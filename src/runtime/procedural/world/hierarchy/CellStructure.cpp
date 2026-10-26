#include "procedural/world/hierarchy/CellStructure.hpp"
#include "procedural/world/hierarchy/FacadeAtlas.hpp"
#include "procedural/world/hierarchy/CellSkeleton.hpp"
#include "procedural/world/hierarchy/RoofFraming.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace procedural::world::hierarchy {

const char *name(const FaceKind kind)
{
  switch (kind)
  {
    case FaceKind::Wall: return "wall";
    case FaceKind::WindowedWall: return "windowed wall";
    case FaceKind::DoorWall: return "door wall";
    case FaceKind::DoubleDoorWall: return "double door wall";
    case FaceKind::ArchWall: return "arch wall";
    case FaceKind::Parapet: return "parapet";
    case FaceKind::Fence: return "fence";
    case FaceKind::Rail: return "rail";
    case FaceKind::Roof: return "roof";
    case FaceKind::Gable: return "gable";
    case FaceKind::Soffit: return "soffit";
    case FaceKind::Deck: return "deck";
    case FaceKind::Cap: return "cap";
    case FaceKind::Pier: return "pier";
    case FaceKind::Post: return "post";
    case FaceKind::Floor: return "floor";
    case FaceKind::Ceiling: return "ceiling";
    case FaceKind::Paving: return "paving";
    case FaceKind::Foundation: return "foundation";
    case FaceKind::Ramp: return "ramp";
    case FaceKind::Landing: return "landing";
    case FaceKind::Timber: return "timber";
    case FaceKind::Furniture: return "furniture";
    case FaceKind::DormerOpening: return "dormer opening";
    case FaceKind::DormerWall: return "dormer wall";
    case FaceKind::DormerWindow: return "dormer window";
    case FaceKind::DormerRoof: return "dormer roof";
    case FaceKind::DormerCeiling: return "dormer ceiling";
    case FaceKind::Count: break;
  }

  return nullptr;
}

const char *name(const Material material)
{
  switch (material)
  {
    case Material::Stone: return "stone";
    case Material::Timber: return "timber";
    case Material::Wattle: return "wattle";
    case Material::Tile: return "tile";
    case Material::Boards: return "boards";
    case Material::Earth: return "earth";
    case Material::Count: break;
  }

  return nullptr;
}

const char *name(const StructureEdgeRole role)
{
  switch (role)
  {
    case StructureEdgeRole::Corner: return "corner";
    case StructureEdgeRole::Eave: return "eave";
    case StructureEdgeRole::Ridge: return "ridge";
    case StructureEdgeRole::Jamb: return "jamb";
    case StructureEdgeRole::Sill: return "sill";
    case StructureEdgeRole::Head: return "head";
    case StructureEdgeRole::Outline: return "outline";
    case StructureEdgeRole::Other: return "other";
  }

  return nullptr;
}

double thicknessFor(const Material material)
{
  // A wall's thickness is its material's. The numbers are the ones the
  // planner's settings carried as `outerWallMetres` and `innerWallMetres`,
  // now attached to the thing that actually decides them.
  switch (material)
  {
    case Material::Stone: return .20;
    case Material::Timber: return .10;
    case Material::Wattle: return .08;
    case Material::Tile: return .03;
    case Material::Boards: return .045;
    case Material::Earth: return 0.;
    case Material::Count: break;
  }

  return 0.;
}

namespace {

// A plain stretch of wall is sliced into segments about this long, and a
// glazed slice carries a human-sized window, bounded by its available wall.
constexpr double kSliceMetres = 1.;
constexpr double kWindowMetres = 1.0;

using rewrite::Position;

bool insideRing(const std::vector<Position> &ring, const double x, const double z)
{
  bool in = false;
  const size_t n = ring.size();

  for (size_t a = 0, b = n - 1; a < n; b = a++)
  {
    const Position &p = ring[a];
    const Position &q = ring[b];

    if ((p.z > z) != (q.z > z) && x < (q.x - p.x) * (z - p.z) / (q.z - p.z) + p.x)
    {
      in = !in;
    }
  }

  return in;
}

} // namespace

Structure buildStructure(const rewrite::DwellingPlan &plan)
{
  Structure out;
  out.coveredPassages=plan.coveredPassages;

  // Object 0 is the outside. Then one object per room.
  {
    StructureObject exterior;
    exterior.name = "exterior";
    exterior.exterior = true;
    out.objects.push_back(exterior);
  }

  std::vector<uint32_t> roomObject(plan.rooms.size(), 0);

  for (size_t r = 0; r < plan.rooms.size(); ++r)
  {
    // A yard or a court is a room of the plan and NOT an object of the
    // structure: it is outside, and the wall facing it is an outside wall.
    // Given an object of its own, every wall along the back of a house came
    // out as an inside wall of the yard.
    if (plan.rooms[r].kind == rewrite::RoomKind::Yard || plan.rooms[r].kind == rewrite::RoomKind::Courtyard ||
        plan.rooms[r].kind == rewrite::RoomKind::Pen)
    {
      continue;
    }

    StructureObject object;
    object.name = rewrite::roomKindName(plan.rooms[r].kind);
    object.exterior = false;
    object.storey = plan.rooms[r].floor;
    object.room = uint32_t(r);
    roomObject[r] = uint32_t(out.objects.size());
    out.objects.push_back(object);
  }

  // Which room a point is in AT A HEIGHT, or the outside. Asked at a height
  // rather than by storey, because a wall between a tall room and a short one
  // is the short room's wall only as high as the short room's ceiling; above
  // that it is an outside wall, and read by storey the tower's side above the
  // nave's roof was an inner wall of the nave, drawn as such, and missing.
  const auto ceilingOf = [&](const size_t r) {
    const rewrite::DwellingRoom &room = plan.rooms[r];
    const double base = room.boundary.empty() ? 0. : room.boundary.front().y;

    if (room.ceilingHeight > 0)
    {
      return room.ceilingHeight;
    }

    return base + (room.floor < plan.floors.size() ? plan.floors[room.floor].height : 3.);
  };

  const auto objectAt = [&](const double x, const double y, const double z) {
    for (size_t r = 0; r < plan.rooms.size(); ++r)
    {
      if (roomObject[r] == 0 || plan.rooms[r].boundary.empty())
      {
        continue;
      }

      const double base = plan.rooms[r].boundary.front().y;

      if (y < base - 1e-6 || y > ceilingOf(r) - 1e-6)
      {
        continue;
      }

      if (insideRing(plan.rooms[r].boundary, x, z))
      {
        return roomObject[r];
      }
    }

    return 0u;
  };

  // The openings, by wall.
  std::map<uint64_t, std::vector<const rewrite::DwellingOpening *>> cuts;

  for (const rewrite::DwellingOpening &cut : plan.openings)
  {
    cuts[cut.wall].push_back(&cut);
  }

  std::map<uint64_t, const rewrite::DwellingDoor *> leafOf;

  for (const rewrite::DwellingDoor &leaf : plan.doors)
  {
    leafOf[leaf.wall] = &leaf;
  }

  uint32_t segment = 0;

  // ---- the walls: one segment per ROOM SIDE, per storey level ------------------
  //
  // The plan's walls are one per lattice edge, because an edge is the unit a
  // door and a room boundary are expressed in. That is not the structure's
  // unit. A wall segment here is the run of wall one room has along one side -
  // the collinear, contiguous edge-walls with the same rooms on both sides -
  // per storey level; a run is split only where a door is cut in it; and each
  // piece of a run carries AT MOST ONE window, placed on the piece rather than
  // inherited from whichever edges the plan glazed. Merged the old way, a
  // room's long side was three walls with three windows in a row, each a cell
  // wide, and the windowed segments read as stripes.

  // Which plan walls meet at each node, on the same storey, so a run's end can
  // be MITRED against the wall that turns the corner there: the two side faces
  // meet at one upright edge, which is then a real edge of the structure.
  struct End
  {
    uint32_t floor = 0;
    int64_t x = 0, z = 0;

    bool operator<(const End &o) const
    {
      return std::tie(floor, x, z) < std::tie(o.floor, o.x, o.z);
    }
  };

  const auto endOf = [](const uint32_t floor, const Position &p) {
    return End{floor, int64_t(std::llround(p.x * 1000.)), int64_t(std::llround(p.z * 1000.))};
  };

  std::map<End, std::vector<size_t>> meets;

  for (size_t w = 0; w < plan.walls.size(); ++w)
  {
    const rewrite::DwellingWall &wall = plan.walls[w];

    if (wall.kind != rewrite::WallKind::Solid || wall.height <= 1e-6)
    {
      continue;
    }

    meets[endOf(wall.floor, wall.a)].push_back(w);
    meets[endOf(wall.floor, wall.b)].push_back(w);
  }

  // 1. The runs.
  struct Run
  {
    std::vector<size_t> walls;
    Position a{}, b{};
    uint32_t floor = 0;
    bool exterior = false;
    bool party = false;
    bool rail = false;
    bool fence = false;
    double height = 0;
    double thickness = 0;
    Material material = Material::Stone;
  };

  struct RunKey
  {
    uint32_t floor;
    int32_t exterior, rail, party;
    int64_t dirX, dirZ, offset, height;
    uint32_t right, left;

    bool operator<(const RunKey &o) const
    {
      return std::tie(floor, exterior, rail, party, dirX, dirZ, offset, height, right, left) <
             std::tie(o.floor, o.exterior, o.rail, o.party, o.dirX, o.dirZ, o.offset, o.height, o.right, o.left);
    }
  };

  std::map<RunKey, std::vector<std::pair<double, size_t>>> byLine; // along -> wall

  for (size_t w = 0; w < plan.walls.size(); ++w)
  {
    const rewrite::DwellingWall &wall = plan.walls[w];
    const double dx = wall.b.x - wall.a.x, dz = wall.b.z - wall.a.z;
    const double length = std::hypot(dx, dz);

    if (length < 1e-6 || wall.height <= 1e-6)
    {
      continue;
    }

    const double ux = dx / length, uz = dz / length;
    const double nx = -uz, nz = ux;
    // `Rail` as well as `Open`: the cell path only ever read `Open`, so a
    // balustrade the plan asked for by name was built as a solid wall.
    const bool rail = wall.kind == rewrite::WallKind::Rail || wall.kind == rewrite::WallKind::Open;
    const bool fence = !rail && wall.exterior && wall.height < 1.6;
    // A fence thin enough to be boards IS boards: a pen's fence is wood, a
    // yard's wall is stone.
    const Material material = rail ? Material::Timber : (fence && wall.thickness <= .12 ? Material::Timber : (plan.facade.boardFacade && !fence ? Material::Boards : plan.facade.timberFacade && wall.a.y>=plan.facade.stoneBaseHeight-1e-6 && !fence ? Material::Wattle : Material::Stone));
    const double thickness = rail ? .08 : (plan.facade.inwardWalls && !fence ? wall.thickness : (fence ? thicknessFor(material) * .6 : thicknessFor(material)));
    const double mx = (wall.a.x + wall.b.x) * .5, mz = (wall.a.z + wall.b.z) * .5;
    const double ask = wall.a.y + std::min(wall.height, 3.) * .5;
    const uint32_t right = objectAt(mx + nx * (thickness * .5 + .05), ask, mz + nz * (thickness * .5 + .05));
    const uint32_t left = objectAt(mx - nx * (thickness * .5 + .05), ask, mz - nz * (thickness * .5 + .05));

    RunKey key;
    key.floor = wall.floor;
    key.exterior = wall.exterior ? 1 : 0;
    key.party = wall.party ? 1 : 0;
    key.rail = rail ? 1 : (fence ? 2 : 0);
    key.dirX = int64_t(std::llround(ux * 1000.));
    key.dirZ = int64_t(std::llround(uz * 1000.));
    key.offset = int64_t(std::llround((wall.a.x * nx + wall.a.z * nz) * 1000.));
    key.height = int64_t(std::llround(wall.height * 1000.));
    key.right = right;
    key.left = left;

    byLine[key].emplace_back(wall.a.x * ux + wall.a.z * uz, w);
  }

  std::vector<Run> runs;

  for (auto &[key, along] : byLine)
  {
    std::sort(along.begin(), along.end());

    Run run;

    for (const auto &[at, w] : along)
    {
      const rewrite::DwellingWall &wall = plan.walls[w];
      const bool joins = !run.walls.empty() && std::hypot(run.b.x - wall.a.x, run.b.z - wall.a.z) < 1e-3;

      if (!joins)
      {
        if (!run.walls.empty())
        {
          runs.push_back(run);
        }

        run = Run();
        run.a = wall.a;
        run.floor = wall.floor;
        run.exterior = wall.exterior;
        run.party = wall.party;
        run.rail = key.rail == 1;
        run.fence = key.rail == 2;
        run.height = wall.height;
        run.material = run.rail || (run.fence && wall.thickness<=.12) ? Material::Timber :
          (plan.facade.boardFacade && !run.fence ? Material::Boards : plan.facade.timberFacade && wall.a.y>=plan.facade.stoneBaseHeight-1e-6 && !run.fence ? Material::Wattle : Material::Stone);
        run.thickness = run.rail ? .08 : (plan.facade.inwardWalls && !run.fence ? wall.thickness : (run.fence ? thicknessFor(run.material) * .6 : thicknessFor(run.material)));
      }

      run.walls.push_back(w);
      run.b = wall.b;
    }

    if (!run.walls.empty())
    {
      runs.push_back(run);
    }
  }

  // 2. Each run into pieces and levels.
  for (const Run &run : runs)
  {
    const double dx = run.b.x - run.a.x, dz = run.b.z - run.a.z;
    const double length = std::hypot(dx, dz);

    if (length < 1e-6)
    {
      continue;
    }

    const double ux = dx / length, uz = dz / length;
    const double nx = -uz, nz = ux;
    const double base = run.a.y;
    const double top = base + run.height;
    const double h = run.thickness * .5;
    const double mx = (run.a.x + run.b.x) * .5, mz = (run.a.z + run.b.z) * .5;
    const bool boundaryWall=plan.facade.inwardWalls && run.exterior && !run.rail && !run.fence;
    const double inward=objectAt(mx+nx*(h+.05),base+std::min(1.,run.height*.5),mz+nz*(h+.05)) ? 1. : -1.;
    const double centreShift=boundaryWall ? inward*h : 0.;

    // The doors along the run, as parameters, and whether the run is glazed
    // at all (any of its edges was).
    struct Span
    {
      double from = 0, to = 0;
      FaceKind kind = FaceKind::Wall;
      double low = 0, high = 0;

      // The opening this tile holds, when it is narrower than the tile. A
      // door is a metre wide and a tile is a cell, so the tile carries the
      // door the way a tile carries a window: the piece stays whole and says
      // where the hole in it is.
      double openFrom = 0, openTo = 0;
    };

    std::vector<Span> doors;
    bool glazed = false;
    double sill = 0, head = 0;

    for (const size_t w : run.walls)
    {
      const auto found = cuts.find(plan.walls[w].id);

      if (found == cuts.end())
      {
        continue;
      }

      for (const rewrite::DwellingOpening *cut : found->second)
      {
        if (cut->kind == rewrite::OpeningKind::Window)
        {
          if (!glazed)
          {
            glazed = true;
            sill = cut->base;
            head = cut->top;
          }

          continue;
        }

        const double t0 = ((cut->a.x - run.a.x) * ux + (cut->a.z - run.a.z) * uz) / length;
        const double t1 = ((cut->b.x - run.a.x) * ux + (cut->b.z - run.a.z) * uz) / length;

        Span door;
        door.from = std::clamp(std::min(t0, t1), 0., 1.);
        door.to = std::clamp(std::max(t0, t1), 0., 1.);
        door.low = cut->base;
        door.high = cut->top;

        const auto leaf = leafOf.find(plan.walls[w].id);
        const bool shut = leaf != leafOf.end() && leaf->second->privateAccess;
        const double width = (door.to - door.from) * length;
        // Openness comes from the passage contract, not its height. Low
        // residential passages must not acquire a facade door leaf.
        door.kind = !shut && (!cut->exterior || cut->top > 2.3) ? FaceKind::ArchWall : ((width > 1.4 || (leaf != leafOf.end() && leaf->second->leaves >= 2)) ? FaceKind::DoubleDoorWall : FaceKind::DoorWall);

        if (door.to - door.from > 1e-6)
        {
          doors.push_back(door);
        }
      }
    }

    std::sort(doors.begin(), doors.end(), [](const Span &p, const Span &q) { return p.from < q.from; });

    for (size_t k = 1; k < doors.size(); ++k)
    {
      doors[k].from = std::max(doors[k].from, doors[k - 1].to);
    }

    // The pieces: plain (or windowed) between the doors, and the doors.
    std::vector<Span> pieces;
    double at = 0;

    // A plain stretch is SLICED into equal segments about a metre long, so a
    // long room side is many small segments and each can carry its own small
    // window. The mesh may join them again if it wants; segments on either
    // side of an internal wall are different runs and never join.
    const auto plainPiece = [&](const double from, const double to) {
      if (to - from <= 1e-6)
      {
        return;
      }

      const double metres = (to - from) * length;

      // Cut on the plan's own lattice: one tile, one cell.
      //
      // The run is a chain of lattice edges, so its ends and every join in it
      // are already on the lattice, and cutting there makes every plain piece
      // exactly one cell wide. The old rule cut it into equal bays of about a
      // metre and a half, which is a module of its own invention: measured,
      // 19% of the structure's wall faces were a whole cell and the rest were
      // whatever a room side divided by its bay happened to be. Cell tiles are
      // what lets a facade swap one for a window or a pair for a door, and it
      // is not a module unless it is the same everywhere.
      //
      // Only a stretch beside a door ends off the lattice, because a door is
      // cut to its own width inside the cell it stands in. That leaves a jamb
      // narrower than a tile, which is absorbed into the tile before it rather
      // than left as a sliver.
      if (plan.cellMetres > .05 && !run.rail && !run.fence)
      {
        const double step = plan.cellMetres / length;
        const size_t first = pieces.size();
        double at = from;

        for (double line = std::ceil(from / step + 1e-7) * step; line < to - 1e-7; line += step)
        {
          Span piece;
          piece.from = at;
          piece.to = line;
          pieces.push_back(piece);
          at = line;
        }

        if (to - at > step * .3 || pieces.size() == first)
        {
          Span piece;
          piece.from = at;
          piece.to = to;
          pieces.push_back(piece);
        }
        else
        {
          pieces.back().to = to;
        }

        return;
      }

      const double spacing = plan.facade.enabled ? std::max(1.2,plan.facade.bayMetres) : kSliceMetres;
      const uint32_t slices = run.rail || run.fence ? 1u : std::max(1u, uint32_t(std::floor(metres / spacing + .5)));

      for (uint32_t k = 0; k < slices; ++k)
      {
        Span piece;
        piece.from = from + (to - from) * double(k) / double(slices);
        piece.to = k + 1 == slices ? to : from + (to - from) * double(k + 1) / double(slices);
        pieces.push_back(piece);
      }
    };

    // A run is a whole number of cells, so it divides into whole tiles - and
    // a door goes INTO the tile it stands in rather than splitting the run
    // where it happens to start. That is what makes the wall a module: every
    // piece the same size, and what it carries - nothing, a window, a door,
    // an arch - a property of the piece.
    if (plan.cellMetres > .05 && !run.rail && !run.fence)
    {
      const uint32_t tiles = std::max(1u, uint32_t(std::llround(length / plan.cellMetres)));

      for (uint32_t k = 0; k < tiles; ++k)
      {
        Span piece;
        piece.from = double(k) / double(tiles);
        piece.to = double(k + 1) / double(tiles);

        for (const Span &door : doors)
        {
          const double lo = std::max(door.from, piece.from);
          const double hi = std::min(door.to, piece.to);

          if (hi - lo <= 1e-6)
          {
            continue;
          }

          // Whichever tile holds most of it takes the whole opening, so a
          // door straddling a lattice line is one door and not two halves.
          if (hi - lo < (door.to - door.from) * .5 - 1e-9)
          {
            continue;
          }

          piece.kind = door.kind;
          piece.low = door.low;
          piece.high = door.high;
          piece.openFrom = door.from;
          piece.openTo = door.to;
        }

        pieces.push_back(piece);
      }
    }
    else
    {
      for (const Span &door : doors)
      {
        plainPiece(at, door.from);

        if (door.to - door.from > 1e-6)
        {
          pieces.push_back(door);
        }

        at = std::max(at, door.to);
      }

      plainPiece(at, 1.);
    }

    bool arcade = plan.facade.enabled && !run.fence && !run.rail &&
      std::any_of(run.walls.begin(),run.walls.end(),[&](size_t w){return std::find(plan.facade.arcadeWalls.begin(),plan.facade.arcadeWalls.end(),plan.walls[w].id)!=plan.facade.arcadeWalls.end();});
    if(plan.facade.corridorSupports && boundaryWall && std::abs(base)<1e-6) {
      auto owner=objectAt(mx+nx*inward*(run.thickness+.05),base+1,mz+nz*inward*(run.thickness+.05));
      if(owner){const auto &room=plan.rooms[out.objects[owner].room];
        arcade=arcade || room.kind==rewrite::RoomKind::Gallery || room.use==rewrite::RoomUse::Corridor;}
    }
    const bool passage = plan.facade.enabled && std::any_of(run.walls.begin(),run.walls.end(),[&](size_t w){return std::find(plan.facade.passageWalls.begin(),plan.facade.passageWalls.end(),plan.walls[w].id)!=plan.facade.passageWalls.end();});
    if(arcade) {
      pieces.clear();plainPiece(0,1);
      for(auto &piece:pieces) {
        bool clear=true;
        for(const auto &flight:plan.stairs) {
          if(flight.external || flight.lowerFloor>=plan.floors.size() ||
             std::abs(plan.floors[flight.lowerFloor].elevation-base)>.05)continue;
          double lo=1e100,hi=-1e100,near=1e100,far=-1e100;
          for(const auto &p:flight.opening) {
            const double along=(p.x-run.a.x)*ux+(p.z-run.a.z)*uz;
            const double normal=(p.x-run.a.x)*nx+(p.z-run.a.z)*nz;
            lo=std::min(lo,along);hi=std::max(hi,along);
            near=std::min(near,normal);far=std::max(far,normal);
          }
          // Decorative arches are not planned doors. Do not invent one
          // beside the solid side of a flight or its supported return leg.
          if(near<1.8 && far> -1.8 && hi>piece.from*length-.4 && lo<piece.to*length+.4)clear=false;
        }
        if(clear){piece.kind=FaceKind::ArchWall;piece.low=0;piece.high=run.height-.3;}
      }
    }
    if(passage) {pieces.clear();Span opening;opening.from=0;opening.to=1;opening.kind=FaceKind::ArchWall;opening.high=run.height-.15;pieces.push_back(opening);}

    // One small window per slice that is long enough, centred on it, where
    // the run was glazed. Decided HERE, on the slice: the plan said which
    // room sides get light, and how many windows and where is the structure's.
    if (glazed && !run.rail && !run.fence)
    {
      // One window every `bay` TILES, not every piece that is long enough.
      //
      // With the run cut on the cell the pieces are all one size, so "long
      // enough" would glaze every one of them and the bay would say nothing.
      // Counted in tiles it says what it always meant: how often a wall is
      // glazed, in the module the wall is built in.
      const uint32_t bay = plan.cellMetres > .05
        ? uint32_t(std::max(1., std::round(std::max(1.2, plan.facade.bayMetres) / plan.cellMetres))) : 1u;
      uint32_t tile = 0;

      for (Span &piece : pieces)
      {
        const double metres = (piece.to - piece.from) * length;
        const bool onBay = piece.kind != FaceKind::Wall || (tile++ % bay) == 0;

        if (!onBay)
        {
          continue;
        }

        const bool turretLight=std::any_of(plan.wallTurrets.begin(),plan.wallTurrets.end(),[&](const auto &t){
          return std::any_of(run.walls.begin(),run.walls.end(),[&](size_t w){
            const auto &wall=plan.walls[w];
            return wall.floor==t.floor && std::any_of(t.footprint.begin(),t.footprint.end(),[&](auto p){return std::hypot(p.x-wall.a.x,p.z-wall.a.z)<1e-6;});
          });
        });
        if (piece.kind != FaceKind::Wall || metres < (turretLight ? .28 : kSliceMetres * .6))
        {
          continue;
        }

        piece.kind = FaceKind::WindowedWall;
        piece.low = sill;
        piece.high = head;
      }
    }

    // The window itself is narrower than its piece: the piece carries it
    // and the mesh cuts it in the middle. The piece is left whole so a room
    // side stays one segment; `low`/`high` say the sill and the head, and
    // the mesh's inset says the width.

    // Levels.
    const double storeyStep = run.floor < plan.floors.size() && plan.floors[run.floor].height > .5
                                ? plan.floors[run.floor].height
                                : 3.;

    std::vector<std::pair<double, double>> levels;

    if (run.rail || run.fence)
    {
      levels.emplace_back(base, top);
    }
    else
    {
      for (double y = base; y < top - 1e-6; y += storeyStep)
      {
        levels.emplace_back(y, std::min(top, y + storeyStep));
      }

      if (levels.size() >= 2 && levels.back().second - levels.back().first < .6)
      {
        levels[levels.size() - 2].second = levels.back().second;
        levels.pop_back();
      }
    }

    // A monumental portal is one opening, even when its head crosses a
    // nominal storey band. Do not insert a lintel or a second door halfway up.
    for(size_t k=0;k+1<levels.size();) {
      bool crosses=false;const double seam=levels[k].second;
      for(const auto &piece:pieces)if(piece.kind==FaceKind::DoorWall || piece.kind==FaceKind::DoubleDoorWall || piece.kind==FaceKind::ArchWall)
        crosses |= base+piece.low<seam && base+piece.high+.25>seam;
      if(crosses){levels[k].second=levels[k+1].second;levels.erase(levels.begin()+k+1);}
      else ++k;
    }

    // How each end of the run closes, per side: mitred against a wall that
    // turns the corner there, else a hair short of half a thickness past the
    // node.
    const double reach = std::max(0., h - .012) / length;

    const auto endParameter = [&](const bool atStart, const double side, const double elevation) {
      const Position &node = atStart ? run.a : run.b;
      const size_t ownIndex = atStart ? run.walls.front() : run.walls.back();
      const auto found = meets.find(endOf(run.floor, node));
      // An interior wall stops at the envelope's INSIDE face.
      //
      // It used to stop at the lattice node, and for a boundary wall the
      // lattice line is its OUTER face - `centreShift` puts the two skins at
      // the line and a whole thickness inside it. So a partition ran through
      // the entire outside wall and was capped flush with the street: a quad
      // its own thickness wide and a storey tall, lying exactly in the
      // exterior skin. That is the narrow dark rectangle standing on a facade,
      // and it is a z-fight rather than a wall.
      double envelope = 0.;

      if (!boundaryWall && plan.facade.inwardWalls && found != meets.end())
      {
        for (const size_t w : found->second)
        {
          if (plan.walls[w].exterior && plan.walls[w].a.y+plan.walls[w].height>elevation)
          {
            envelope = std::max(envelope, plan.walls[w].thickness);
          }
        }
      }

      const bool atEnvelope = envelope > 0.;
      const double flush = atStart ? envelope / length : 1. - envelope / length;
      const double plain = boundaryWall ? (atStart?0.:1.) : atEnvelope ? flush : (atStart ? -reach : 1. + reach);

      if (run.rail || found == meets.end())
      {
        return plain;
      }

      // A COLLINEAR neighbour is not a corner, however many walls meet here.
      //
      // `plain` extends a non-boundary run by `reach` past each end so its
      // face closes where a perpendicular wall turns. Where another wall
      // continues this run's own line - which is why the run was split, the
      // rooms behind it changed - there is nothing to close, and extending
      // both ends made the two faces overlap by 2*reach over their whole
      // height: two surfaces in one plane at one depth, the doubled vertical
      // seam that stipples down a wall.
      //
      // The test has to run before the "exactly two walls meet" gate below,
      // because the split is usually caused by a partition arriving at the
      // same node - so three walls meet there, not two, and the gate sent
      // every one of these straight back to `plain`.
      for (const size_t w : found->second)
      {
        if (w == ownIndex || plan.walls[w].a.y+plan.walls[w].height<=elevation)
        {
          continue;
        }

        const rewrite::DwellingWall &line = plan.walls[w];
        const double lx = line.b.x - line.a.x, lz = line.b.z - line.a.z;
        const double llen = std::hypot(lx, lz);

        if (line.exterior == run.exterior && llen > 1e-6 && std::abs((ux * lx + uz * lz) / llen) > .999)
        {
          return boundaryWall ? (atStart ? 0. : 1.) : flush;
        }
      }

      // A partition arriving at an exterior corner must not disable the
      // corner miter: its two inward skins otherwise leave an open slit.
      size_t otherIndex = plan.walls.size();
      const bool sameKind=std::any_of(found->second.begin(),found->second.end(),[&](size_t i){
        return i!=ownIndex && plan.walls[i].exterior==run.exterior && plan.walls[i].a.y+plan.walls[i].height>elevation;
      });
      for(const auto candidate:found->second)if(candidate!=ownIndex) {
        const auto &wall=plan.walls[candidate];
        if(wall.a.y+wall.height<=elevation)continue;
        if(sameKind && wall.exterior!=run.exterior)continue;
        if(wall.exterior!=run.exterior && wall.a.y+wall.height<top-1e-6)continue;
        if(otherIndex!=plan.walls.size()){otherIndex=plan.walls.size();break;}
        otherIndex=candidate;
      }
      if(otherIndex==plan.walls.size())return plain;
      const rewrite::DwellingWall &other = plan.walls[otherIndex];


      const double ox = other.b.x - other.a.x, oz = other.b.z - other.a.z;
      const double olen = std::hypot(ox, oz);

      // Above a lower adjoining room, an interior tower wall meets an
      // exterior wall alone. Join their actual offset skins, even though
      // their thicknesses and centrelines differ.
      if (olen>1e-6 && (other.exterior!=run.exterior || std::abs(other.thickness-run.thickness)>1e-6)) {
        const double oux=ox/olen,ouz=oz/olen,onx=-ouz,onz=oux;
        const bool starts=std::hypot(other.a.x-node.x,other.a.z-node.z)<1e-3;
        const double awayX=starts?oux:-oux,awayZ=starts?ouz:-ouz;
        const double ownX=atStart?ux:-ux,ownZ=atStart?uz:-uz;
        const double otherSide=((nx*side*awayX+nz*side*awayZ)>0?1.:-1.) *
                               ((onx*ownX+onz*ownZ)>0?1.:-1.);
        const double oh=other.thickness*.5;
        const double omx=(other.a.x+other.b.x)*.5,omz=(other.a.z+other.b.z)*.5;
        const double oi=objectAt(omx+onx*(oh+.05),elevation,omz+onz*(oh+.05))?1.:-1.;
        const double shift=plan.facade.inwardWalls && other.exterior ? oi*oh : 0.;
        const double offset=centreShift+side*h,otherOffset=shift+otherSide*oh;
        const double cross=ux*ouz-uz*oux;
        if(std::abs(cross)>1e-8) {
          const double qx=onx*otherOffset-nx*offset,qz=onz*otherOffset-nz*offset;
          return (atStart?0.:1.)+(qx*ouz-qz*oux)/cross/length;
        }
      }

      if(boundaryWall && olen>1e-6) {
        const double oux=ox/olen,ouz=oz/olen,onx=-ouz,onz=oux;
        const double omx=(other.a.x+other.b.x)*.5,omz=(other.a.z+other.b.z)*.5;
        const double oi=objectAt(omx+onx*(h+.05),base+std::min(1.,run.height*.5),omz+onz*(h+.05))?1.:-1.;
        const double offset=centreShift+side*h;
        const double otherOffset=side*inward>0 ? oi*run.thickness : 0.;
        const double cross=ux*ouz-uz*oux;
        if(std::abs(cross)<1e-8)return plain;
        const double qx=onx*otherOffset-nx*offset,qz=onz*otherOffset-nz*offset;
        return (atStart?0.:1.)+(qx*ouz-qz*oux)/cross/length;
      }

      if (olen < 1e-6)
      {
        return plain;
      }

      if (std::abs((ux * ox + uz * oz) / olen) > 1e-3)
      {
        return plain;
      }

      const bool otherStartsHere = std::abs(other.a.x - node.x) < 1e-3 && std::abs(other.a.z - node.z) < 1e-3;
      const double ax = (otherStartsHere ? ox : -ox) / olen, az = (otherStartsHere ? oz : -oz) / olen;
      const double sx = nx * side, sz = nz * side;
      const double along = (sx * ax + sz * az) > 0 ? h : -h;
      const double shift = along / length;

      return atStart ? 0. + shift : 1. - shift;
    };

    for (const auto &[levelBase, levelTop] : levels)
    {
      const double ask = (levelBase + levelTop) * .5;
      const double rightStart = endParameter(true, 1., ask);
      const double rightFinish = endParameter(false, 1., ask);
      const double leftStart = endParameter(true, -1., ask);
      const double leftFinish = endParameter(false, -1., ask);
      const uint32_t rightObject = objectAt(mx + nx * (h + .05), ask, mz + nz * (h + .05));
      const uint32_t leftObject = objectAt(mx - nx * (h + .05), ask, mz - nz * (h + .05));

      for (const Span &piece : pieces)
      {
        // What this level carries: the cut where it falls inside the level,
        // clamped a course under the level's top; nothing where it does not.
        double low = 0, high = 0;
        FaceKind carriedKind = piece.kind;

        if (piece.kind != FaceKind::Wall)
        {
          const double cutLow = base + piece.low, cutHigh = base + piece.high;

          if (cutHigh <= levelBase + .05 || cutLow >= levelTop - .3)
          {
            carriedKind = FaceKind::Wall;
          }
          else
          {
            low = std::max(0., cutLow - levelBase);
            high = std::min(levelTop - levelBase - (passage?.15:.25), cutHigh - levelBase);

            if (high - low < .3)
            {
              carriedKind = FaceKind::Wall;
              low = high = 0;
            }
          }
        }

        // A window on every storey of a glazed side, not only the first: the
        // plan glazed the ground storey's edge, and the storeys above are the
        // same room side of the same run.
        if (piece.kind == FaceKind::WindowedWall && carriedKind == FaceKind::Wall && levelTop - levelBase > 2.2 && glazed)
        {
          carriedKind = FaceKind::WindowedWall;
          low = sill;
          high = std::min(levelTop - levelBase - .25, head);
        }

        // Where along the piece the carried thing is, as fractions of the
        // RUN for now; each side turns them into fractions of its own base
        // edge below, since a side may reach past the piece at a mitre and
        // the inside runs the other way. A window is a window's width - a
        // metre and a bit, never a share of the wall - centred on its piece;
        // a door piece is the door's own width, less a hair of jamb each side.
        double along0 = 0, along1 = 0;

        if (carriedKind == FaceKind::WindowedWall)
        {
          const double metres = (piece.to - piece.from) * length;
          const double desired = plan.facade.enabled && plan.facade.style!=rewrite::FacadeStyle::Domestic ? 1.15 : plan.facade.windowWidth;
          // Reserve physical jambs, not forty percent of every cell.
          // Percentage margins made windows shrink with the planning grid.
          const double width = std::min(desired, std::max(0.,metres-.24));
          const double centre = (piece.from + piece.to) * .5;
          along0 = centre - width * .5 / length;
          along1 = centre + width * .5 / length;
        }
        else if (carriedKind != FaceKind::Wall)
        {
          const double jamb = (arcade || passage ? .2 : .06) / length;

          if (piece.openTo > piece.openFrom + 1e-9 && !arcade && !passage)
          {
            // A door's own width inside its tile, never the tile's width: a
            // cell is two metres and a bit, and a doorway that wide is a
            // cart entrance.
            along0 = std::max(piece.from + jamb, piece.openFrom);
            along1 = std::min(piece.to - jamb, piece.openTo);
          }
          else
          {
            along0 = piece.from + jamb;
            along1 = piece.to - jamb;
          }
        }

        FaceKind kind = run.rail ? FaceKind::Rail : (run.fence ? FaceKind::Fence : carriedKind);
        const bool first = piece.from <= 1e-9;
        const bool last = piece.to >= 1. - 1e-9;
        const double r0 = first ? rightStart : piece.from, r1 = last ? rightFinish : piece.to;
        const double l0 = first ? leftStart : piece.from, l1 = last ? leftFinish : piece.to;
        if(kind==FaceKind::WindowedWall || kind==FaceKind::ArchWall || kind==FaceKind::DoorWall || kind==FaceKind::DoubleDoorWall) {
          // Both skins share one physical aperture. An inward corner can
          // shorten the inside skin; never extend its opening past that edge.
          const double available0=std::max(r0,l0)+.005/length;
          const double available1=std::min(r1,l1)-.005/length;
          const double requested=along1-along0;
          // Move the whole aperture away from an inward corner before
          // shortening it. Both wall skins must retain the same dimensions.
          if(available1-available0>=requested) {
            along0=std::clamp(along0,available0,available1-requested);along1=along0+requested;
          } else {along0=available0;along1=available1;}
          if(along1<=along0){kind=FaceKind::Wall;low=high=along0=along1=0;}
        }
        const double x0 = run.a.x + dx * r0, z0 = run.a.z + dz * r0;
        const double x1 = run.a.x + dx * r1, z1 = run.a.z + dz * r1;
        const double lx0 = run.a.x + dx * l0, lz0 = run.a.z + dz * l0;
        const double lx1 = run.a.x + dx * l1, lz1 = run.a.z + dz * l1;

        StructureFace right;
        right.kind = kind;
        right.material = run.material;
        right.object = rightObject;
        right.segment = segment;
        right.party = run.party;
        right.thickness = run.thickness;
        right.passagePortal=passage;
        if(arcade) {right.leftEdge=FacadeEdge::PillarRightHalf;right.rightEdge=FacadeEdge::PillarLeftHalf;}
        right.low = low;
        right.high = high;
        right.along0 = (along0 - r0) / (r1 - r0);
        right.along1 = (along1 - r0) / (r1 - r0);
        right.outline = {Position{x0 + nx * (centreShift+h), levelBase, z0 + nz * (centreShift+h)}, Position{x1 + nx * (centreShift+h), levelBase, z1 + nz * (centreShift+h)},
                         Position{x1 + nx * (centreShift+h), levelTop, z1 + nz * (centreShift+h)}, Position{x0 + nx * (centreShift+h), levelTop, z0 + nz * (centreShift+h)}};
        out.faces.push_back(std::move(right));

        StructureFace left;
        left.kind = kind;
        left.material = run.material;
        left.object = leftObject;
        left.segment = segment;
        left.party = run.party;
        left.thickness = run.thickness;
        left.passagePortal=passage;
        if(arcade) {left.leftEdge=FacadeEdge::PillarRightHalf;left.rightEdge=FacadeEdge::PillarLeftHalf;}
        left.low = low;
        left.high = high;
        left.along0 = (l1 - along1) / (l1 - l0);
        left.along1 = (l1 - along0) / (l1 - l0);
        left.outline = {Position{lx1 + nx * (centreShift-h), levelBase, lz1 + nz * (centreShift-h)}, Position{lx0 + nx * (centreShift-h), levelBase, lz0 + nz * (centreShift-h)},
                        Position{lx0 + nx * (centreShift-h), levelTop, lz0 + nz * (centreShift-h)}, Position{lx1 + nx * (centreShift-h), levelTop, lz1 + nz * (centreShift-h)}};
        out.faces.push_back(std::move(left));

        ++segment;
      }
    }
  }

  // ---- everything that is not a wall -------------------------------------------
  //
  // Each of these is one plan record becoming one or more typed faces, with
  // the object it belongs to. Nothing here decides how it is drawn.
  const auto face = [&](const FaceKind kind, const Material material, const uint32_t object, std::vector<Position> outline) {
    if (outline.size() < 3)
    {
      return;
    }

    StructureFace f;
    f.kind = kind;
    f.material = material;
    f.object = object;
    f.outline = std::move(outline);
    out.faces.push_back(std::move(f));
  };

  // A prism's sides, one face per edge of its footprint.
  const auto band = [&](const FaceKind kind, const Material material, const uint32_t object,
                        const std::vector<Position> &ring, const double base, const double top) {
    for (size_t k = 0; k < ring.size(); ++k)
    {
      const Position &a = ring[k];
      const Position &b = ring[(k + 1) % ring.size()];

      if (std::hypot(b.x - a.x, b.z - a.z) < 1e-6)
      {
        continue;
      }

      face(kind, material, object, {Position{a.x, base, a.z}, Position{b.x, base, b.z}, Position{b.x, top, b.z}, Position{a.x, top, a.z}});
    }
  };

  std::vector<uint32_t> roofStructureFaces;
  roofStructureFaces.reserve(plan.roofs.size());

  for (const rewrite::DwellingRoofFace &roof : plan.roofs)
  {
    bool flat = true;

    for (const Position &q : roof.polygon)
    {
      flat = flat && std::abs(q.y - roof.polygon.front().y) < 1e-6;
    }

    const FaceKind kind = roof.soffit ? FaceKind::Soffit : roof.gable ? FaceKind::Gable : flat ? FaceKind::Deck : FaceKind::Roof;
    roofStructureFaces.push_back(uint32_t(out.faces.size()));
    face(kind, roof.soffit ? Material::Timber : roof.gable ? (plan.facade.boardFacade ? Material::Boards : plan.facade.timberFacade ? Material::Wattle : Material::Stone) : Material::Tile, 0, roof.polygon);
    out.faces.back().radialRoof=roof.radial;
  }

  // One component per dormer. The opening is structural topology rather than
  // a drawable face: it records the aperture and links the projection to all
  // three pieces of roof left around it.
  for (uint32_t component = 0; component < uint32_t(plan.dormers.size()); ++component)
  {
    const rewrite::DwellingDormer &dormer = plan.dormers[component];
    std::vector<uint32_t> supports;
    for (const uint32_t roof : dormer.roofs)
    {
      if (roof < roofStructureFaces.size()) supports.push_back(roofStructureFaces[roof]);
    }
    const uint32_t support = supports.empty() ? UINT32_MAX : supports.front();
    const auto dormerFace = [&](const FaceKind kind, const Material material, const std::vector<Position> &outline) {
      if (outline.size() < 3) return;
      StructureFace f;
      f.kind = kind;
      f.material = material;
      f.object = 0;
      f.outline = outline;
      f.component = component;
      f.support = support;
      f.supports = supports;
      out.faces.push_back(std::move(f));
    };

    dormerFace(FaceKind::DormerOpening, Material::Timber, dormer.opening);
    for (const auto &wall : dormer.walls) dormerFace(FaceKind::DormerWall, Material::Timber, wall);
    for (const auto &roof : dormer.roofFaces) dormerFace(FaceKind::DormerRoof, Material::Tile, roof);
    dormerFace(FaceKind::DormerWindow, Material::Timber, dormer.window);
    dormerFace(FaceKind::DormerCeiling, Material::Boards, dormer.ceiling);
  }

  // Roman courts already have explicit cell walls and opening cuts. A second
  // continuous band would seal their doors and windows.
  // Legacy outline-only plans still need their inner wall band.
  if(plan.architecture!=rewrite::DwellingArchitecture::Roman)
  for (const rewrite::DwellingStorey &storey : plan.storeys)
  {
    if (storey.top <= 1e-6)
    {
      continue;
    }

    for (const auto &hole : storey.holes)
    {
      band(FaceKind::Wall, Material::Stone, 0, hole, storey.base, storey.top);
    }
  }

  for (const rewrite::DwellingPillar &post : plan.pillars)
  {
    band(post.masonry ? FaceKind::Pier : FaceKind::Post, post.masonry ? Material::Stone : Material::Timber, 0, post.footprint, post.base, post.top);
    if(post.capped) {
      auto cap=post.footprint;for(auto &q:cap)q.y=post.top;face(FaceKind::Pier,Material::Stone,0,cap);
      std::reverse(cap.begin(),cap.end());for(auto &q:cap)q.y=post.base;face(FaceKind::Pier,Material::Stone,0,cap);
    }
  }

  for (const rewrite::DwellingPillar &stack : plan.chimneys)
  {
    band(FaceKind::Pier, Material::Stone, 0, stack.footprint, stack.base, stack.top);
  }

  // Corbel courses close underneath the projecting chamber. Emit only the
  // exposed shoulder of each course, never coincident caps between courses.
  for (const auto &turret : plan.wallTurrets)
  {
    const auto &ring=turret.footprint;
    if(ring.size()<3) continue;
    const Position anchor{(ring.front().x+ring.back().x)*.5,turret.base,(ring.front().z+ring.back().z)*.5};
    const auto scaled=[&](double scale,double y) {
      std::vector<Position> out;
      for(auto p:ring)out.push_back({anchor.x+(p.x-anchor.x)*scale,y,anchor.z+(p.z-anchor.z)*scale});
      return out;
    };
    auto previous=scaled(.25,turret.corbelBase);
    face(FaceKind::Pier,Material::Stone,0,previous);
    for(int course=0;course<6;++course) {
      const double y=turret.corbelBase+(turret.base-turret.corbelBase)*course/6.;
      const double top=turret.corbelBase+(turret.base-turret.corbelBase)*(course+1)/6.;
      auto outer=scaled(.25+.75*(course+1)/6.,y);
      for(size_t k=0;k+1<ring.size();++k) {
        const size_t n=k+1;
        previous[k].y=previous[n].y=y;
        face(FaceKind::Pier,Material::Stone,0,{previous[k],previous[n],outer[n],outer[k]});
        face(FaceKind::Pier,Material::Stone,0,{outer[k],outer[n],{outer[n].x,top,outer[n].z},{outer[k].x,top,outer[k].z}});
      }
      previous=outer;
    }
  }

  // A ceiling is a thin solid, not an infinitely thin lid. Only exposed
  // perimeter edges receive a rim; shared cell edges never become seams.
  struct CeilingEdge { Position a, b; uint32_t owner; unsigned count=0; };
  using CeilingKey = std::tuple<uint32_t,int64_t,int64_t,int64_t,int64_t,int64_t>;
  std::map<CeilingKey,CeilingEdge> ceilingEdges;
  constexpr double ceilingThickness=.12, ceilingClearance=.05;

  // A ceiling under a storey hangs clear of its boards.
  //
  // A ceiling is a slab: a face looking up, a face looking down and a rim. Its
  // top sat at the head of the storey, which is exactly the elevation of the
  // floor above - the same plane, over the same ground, and the depth test
  // cannot choose between them. Measured over the gallery that was 1486 m2 of
  // the 1956 m2 of coplanar surface in the whole corpus, and it is the stipple
  // across every intermediate floor.
  //
  // The slab keeps both its skins and hangs a void below the boards instead,
  // which is what a floor actually is: boards, a void, then the ceiling under
  // them. Five centimetres and not two, because a foundation's top is inset
  // exactly two below the finish layer - dropping the ceiling by the same
  // number simply moved it from one plane it shared to another, and put 217
  // m2 of ceiling into the foundations.
  std::map<int64_t, std::vector<const std::vector<Position> *>> boardsAt;

  for (const rewrite::DwellingFloor &floor : plan.floors)
  {
    for (const auto &piece : floor.slabs)
    {
      if (piece.size() >= 3)
      {
        boardsAt[std::llround(piece.front().y * 1000.)].push_back(&piece);
      }
    }
  }

  const auto flooredOver = [&](const std::vector<Position> &tile) {
    const auto found = boardsAt.find(std::llround(tile.front().y * 1000.));

    if (found == boardsAt.end())
    {
      return false;
    }

    double x = 0, z = 0;

    for (const Position &p : tile)
    {
      x += p.x;
      z += p.z;
    }

    x /= double(tile.size());
    z /= double(tile.size());

    for (const std::vector<Position> *piece : found->second)
    {
      if (insideRing(*piece, x, z))
      {
        return true;
      }
    }

    return false;
  };

  for (const auto &ceiling : plan.ceilings)
  {
    if (ceiling.size() < 3) continue;
    double x = 0, z = 0;
    for (const auto &p : ceiling) { x += p.x; z += p.z; }
    x /= ceiling.size(); z /= ceiling.size();
    const auto owner=objectAt(x, ceiling.front().y - 1e-4, z);
    if(owner && plan.rooms[out.objects[owner].room].openToRoof) continue;
    auto underside=ceiling;

    if (flooredOver(ceiling))
    {
      for (Position &p : underside)
      {
        p.y -= ceilingClearance;
      }
    }

    double signedArea=0;
    // Exposed joists belong to the actual ceiling slab, so stair openings
    // remain clear. Restrict this detail to rectangular, horizontal panels.
    if(!plan.facade.roofFraming && owner && underside.size()>=4) {
      double x0=1e30,x1=-1e30,z0=1e30,z1=-1e30,twiceArea=0;bool rectangular=true;
      for(size_t i=0;i<underside.size();++i) {
        auto a=underside[i],b=underside[(i+1)%underside.size()];
        twiceArea+=a.x*b.z-b.x*a.z;
        x0=std::min(x0,a.x);x1=std::max(x1,a.x);z0=std::min(z0,a.z);z1=std::max(z1,a.z);
        rectangular &= std::abs(a.y-underside[0].y)<1e-6 && (std::abs(a.x-b.x)<1e-6 || std::abs(a.z-b.z)<1e-6);
      }
      rectangular &= std::abs(std::abs(twiceArea)*.5-(x1-x0)*(z1-z0))<1e-6;
      const double top=underside[0].y-ceilingThickness;
      const auto &room=plan.rooms[out.objects[owner].room];
      const double floor=room.boundary.empty()?top:room.boundary.front().y;
      bool framed=false;
      for(const auto &t:plan.timbers)if(std::abs(t.a.y-top)<.35 && std::abs(t.b.y-top)<.35 &&
          std::min(t.a.x,t.b.x)<=x1 && std::max(t.a.x,t.b.x)>=x0 &&
          std::min(t.a.z,t.b.z)<=z1 && std::max(t.a.z,t.b.z)>=z0)framed=true;
      if(rectangular && !framed && top-floor>2.35 && x1-x0>1.2 && z1-z0>1.2) {
        const bool alongX=x1-x0<=z1-z0;
        const double start=alongX?z0:x0,end=alongX?z1:x1;
        for(double station=std::ceil((start+.12)/.75)*.75;station<end-.12;station+=.75) {
          const double a=alongX?x0:station-.05,b=alongX?x1:station+.05;
          const double c=alongX?station-.05:z0,d=alongX?station+.05:z1;
          const Position v[8]={{a,top,c},{b,top,c},{b,top,d},{a,top,d},
            {a,top-.16,c},{b,top-.16,c},{b,top-.16,d},{a,top-.16,d}};
          for(const auto indices:std::vector<std::array<int,4>>{{4,5,6,7},{0,4,7,3},{1,2,6,5},{0,1,5,4},{3,7,6,2}})
            face(FaceKind::Timber,Material::Timber,owner,{v[indices[0]],v[indices[1]],v[indices[2]],v[indices[3]]});
        }
      }
    }
    for(size_t i=0;i<underside.size();++i) {
      auto a=underside[i],b=underside[(i+1)%underside.size()];
      signedArea+=a.x*b.z-b.x*a.z;
    }
    if(signedArea<0)std::reverse(underside.begin(),underside.end());
    auto top=underside;std::reverse(top.begin(),top.end());
    face(FaceKind::Ceiling,Material::Boards,owner,top);
    for(auto &p:underside)p.y-=ceilingThickness;
    face(FaceKind::Ceiling,Material::Boards,owner,underside);
    for(size_t i=0;i<underside.size();++i) {
      auto a=underside[i],b=underside[(i+1)%underside.size()];
      a.y+=ceilingThickness;b.y+=ceilingThickness;
      const auto keyPoint=[](Position p){return std::pair<int64_t,int64_t>{std::llround(p.x*1000000),std::llround(p.z*1000000)};};
      auto ka=keyPoint(a),kb=keyPoint(b);if(kb<ka)std::swap(ka,kb);
      auto &edge=ceilingEdges[{owner,std::llround(a.y*1000000),ka.first,ka.second,kb.first,kb.second}];
      edge.a=a;edge.b=b;edge.owner=owner;++edge.count;
    }
  }
  // A ceiling rim only where the slab's edge is actually exposed.
  //
  // The rim closes the twelve centimetres of slab you would otherwise see the
  // inside of - at a stair well, or where a ceiling simply stops. Against a
  // wall there is nothing to see and the rim lands IN the wall's own skin,
  // because the ceiling is laid to the wall's inner face: two surfaces in one
  // plane at one depth, over the top twelve centimetres of every wall in the
  // building. Measured, 224 m2 against walls and another 98 m2 of one room's
  // rim against the next room's along a shared partition.
  const auto againstAWall = [&](const Position &a, const Position &b) {
    const double mx = (a.x + b.x) * .5, mz = (a.z + b.z) * .5;

    for (const rewrite::DwellingWall &wall : plan.walls)
    {
      if (a.y < wall.a.y - .02 || a.y > wall.a.y + wall.height + .02)
      {
        continue;
      }

      const double wx = wall.b.x - wall.a.x, wz = wall.b.z - wall.a.z;
      const double len = std::hypot(wx, wz);

      if (len < 1e-6)
      {
        continue;
      }

      // Distance from the rim's midpoint to the wall's segment, and it must
      // be beside the wall rather than off the end of it.
      const double t = std::clamp(((mx - wall.a.x) * wx + (mz - wall.a.z) * wz) / (len * len), 0., 1.);
      const double dx = mx - (wall.a.x + wx * t), dz = mz - (wall.a.z + wz * t);

      // A whole thickness, not half: a boundary wall is SHIFTED off its
      // plan line - `centreShift` puts its skins at the line and a thickness
      // inside it - so the inner skin a ceiling is laid to is a full
      // thickness from the line, and half of one never reached it.
      if (std::hypot(dx, dz) <= wall.thickness + .03)
      {
        return true;
      }
    }

    return false;
  };

  for(const auto &[key,edge]:ceilingEdges)if(edge.count==1) {
    if (againstAWall(edge.a, edge.b))
    {
      continue;
    }

    auto a=edge.a,b=edge.b,ad=a,bd=b;ad.y-=ceilingThickness;bd.y-=ceilingThickness;
    face(FaceKind::Ceiling,Material::Boards,edge.owner,{a,b,bd,ad});
  }

  for (const rewrite::DwellingFloor &floor : plan.floors)
  {
    if (floor.elevation < -1e-6)
    {
      continue;
    }

    for (const auto &piece : floor.slabs)
    {
      // A floor belongs to the room it is in, or to the outside when no room
      // claims it (a gallery, a balcony deck).
      double cx = 0, cz = 0;

      for (const Position &q : piece)
      {
        cx += q.x;
        cz += q.z;
      }

      cx /= double(std::max<size_t>(1, piece.size()));
      cz /= double(std::max<size_t>(1, piece.size()));

      uint32_t owner = 0;

      for (size_t r = 0; r < plan.rooms.size(); ++r)
      {
        if (std::abs(plan.rooms[r].boundary.empty() ? -1e300 : plan.rooms[r].boundary.front().y - floor.elevation) < 1e-6 &&
            insideRing(plan.rooms[r].boundary, cx, cz))
        {
          owner = roomObject[r];
          break;
        }
      }

      face(FaceKind::Floor, Material::Boards, owner, piece);
    }

    for (const auto &piece : floor.paving)
    {
      face(FaceKind::Paving, Material::Earth, 0, piece);
    }
  }

  // A flight is one inclined rectangle from its foot to its head, and a
  // landing is one level pad. The simple version, on purpose: what the mesh
  // needs from a stair is a surface you can walk up and a place to step off,
  // and the treads are a later mesh step's to cut into the ramp if it wants
  // them. The opening's first edge is the foot and its far edge the head.
  for (const rewrite::DwellingStair &flight : plan.stairs)
  {
    if (flight.opening.size() < 4)
    {
      continue;
    }

    const double base = (flight.lowerFloor < plan.floors.size() ? plan.floors[flight.lowerFloor].elevation : 0.) + flight.startRise;

    if (base + flight.rise <= .12)
    {
      continue;
    }

    const Position &p0 = flight.opening[0];
    const Position &p1 = flight.opening[1];
    const Position &p2 = flight.opening[2];
    const Position &p3 = flight.opening[3];

    face(FaceKind::Ramp, Material::Timber, 0,
         {Position{p0.x, base, p0.z}, Position{p1.x, base, p1.z}, Position{p2.x, base + flight.rise, p2.z},
          Position{p3.x, base + flight.rise, p3.z}});
    out.faces.back().stairRisers=std::max(2u,flight.risers);
    out.faces.back().stairSupportBase=base-flight.startRise;
    out.faces.back().stairWallSupported=flight.wallSupported;

    for (const std::vector<Position> *pad : {&flight.lowerLanding, &flight.upperLanding})
    {
      if (pad->size() < 3)
      {
        continue;
      }

      const double y = pad == &flight.lowerLanding ? base : base + flight.rise;
      std::vector<Position> flat;

      for (const Position &q : *pad)
      {
        flat.push_back(Position{q.x, y, q.z});
      }

      face(FaceKind::Landing, Material::Timber, 0, flat);
      // Intermediate landings are carried to this storey's floor; arrival
      // bridges keep headroom for the doorway underneath.
      const double floorBase=base-flight.startRise;
      const double floorTop=flight.lowerFloor<plan.floors.size()?floorBase+plan.floors[flight.lowerFloor].height:y;
      const double bottom=flight.wallSupported ? y-.18 : (y<floorTop-.1?floorBase:y-.18);
      auto underside=flat;for(auto &p:underside)p.y=bottom;
      std::reverse(underside.begin(),underside.end());face(FaceKind::Timber,Material::Timber,0,underside);
      for(size_t k=0;k<flat.size();++k){auto a=flat[k],b=flat[(k+1)%flat.size()];auto c=b,d=a;c.y=d.y=bottom;
        face(FaceKind::Timber,Material::Timber,0,{a,d,c,b});}

    }
  }

  for (const rewrite::DwellingTimber &member : plan.timbers)
  {
    const double dx = member.b.x - member.a.x, dy = member.b.y - member.a.y, dz = member.b.z - member.a.z;
    const double run = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (run < 1e-6)
    {
      continue;
    }

    double px = -dz, py = 0, pz = dx;
    double plen = std::hypot(px, pz);

    if (plen < 1e-6)
    {
      px = 1;
      pz = 0;
      plen = 1;
    }

    px = px / plen * member.width * .5;
    pz = pz / plen * member.width * .5;
    const double ux = dx / run, uy = dy / run, uz = dz / run;
    double qx = uy * pz - uz * py, qy = uz * px - ux * pz, qz = ux * py - uy * px;
    const double qlen = std::sqrt(qx * qx + qy * qy + qz * qz);

    if (qlen < 1e-6)
    {
      continue;
    }

    qx = qx / qlen * member.depth * .5;
    qy = qy / qlen * member.depth * .5;
    qz = qz / qlen * member.depth * .5;

    const auto at = [&](const Position &end, const int32_t sp, const int32_t sq) {
      return Position{end.x + px * sp + qx * sq, end.y + py * sp + qy * sq, end.z + pz * sp + qz * sq};
    };

    const int32_t sp[4] = {1, -1, -1, 1};
    const int32_t sq[4] = {1, 1, -1, -1};

    for (int32_t k = 0; k < 4; ++k)
    {
      const int32_t n = (k + 1) % 4;
      face(FaceKind::Timber, Material::Timber, 0, {at(member.a, sp[k], sq[k]), at(member.a, sp[n], sq[n]), at(member.b, sp[n], sq[n]), at(member.b, sp[k], sq[k])});
    }
    std::vector<Position> start,end;
    for(int k=0;k<4;++k){start.push_back(at(member.a,sp[k],sq[k]));end.push_back(at(member.b,sp[k],sq[k]));}
    std::reverse(start.begin(),start.end());
    face(FaceKind::Timber,Material::Timber,0,start);
    face(FaceKind::Timber,Material::Timber,0,end);
  }

  // Furniture is structural data before it is a mesh: one typed box per
  // planned item, with all six polygons sharing a component identity. The
  // simple mesh merely draws those polygons; a future detail pass can replace
  // a Bed box with a real bed without consulting the blueprint again.
  const auto furnitureHeight = [](const rewrite::FurnitureKind kind) {
    switch (kind)
    {
      case rewrite::FurnitureKind::Bed: return .55;
      case rewrite::FurnitureKind::Table: return .75;
      case rewrite::FurnitureKind::Counter: return 1.0;
      case rewrite::FurnitureKind::Hearth: return 1.15;
      case rewrite::FurnitureKind::Chest: return .8;
      case rewrite::FurnitureKind::Bench: return .5;
      case rewrite::FurnitureKind::Workbench: return .9;
    }

    return .75;
  };

  uint32_t furnitureComponent = 0;

  for (const rewrite::DwellingFurniture &item : plan.furniture)
  {
    if (item.footprint.size() < 3)
    {
      continue;
    }

    uint32_t owner = 0;

    for (uint32_t r = 0; r < uint32_t(plan.rooms.size()); ++r)
    {
      if (plan.rooms[r].id == item.room)
      {
        owner = roomObject[r];
        break;
      }
    }

    const double base = item.footprint.front().y;
    const double top = base + furnitureHeight(item.kind);
    std::vector<Position> bottom, upper;

    for (const Position &p : item.footprint)
    {
      bottom.push_back(Position{p.x, base, p.z});
      upper.push_back(Position{p.x, top, p.z});
    }

    const auto furnitureFace = [&](std::vector<Position> outline) {
      StructureFace f;
      f.kind = FaceKind::Furniture;
      f.material = item.kind == rewrite::FurnitureKind::Hearth ? Material::Stone : Material::Timber;
      f.object = owner;
      f.outline = std::move(outline);
      f.component = furnitureComponent;
      f.furnitureKind = item.kind;
      f.facing = item.facing;
      out.faces.push_back(std::move(f));
    };

    std::reverse(bottom.begin(), bottom.end());
    furnitureFace(bottom);
    furnitureFace(upper);

    for (size_t k = 0; k < item.footprint.size(); ++k)
    {
      const Position &a = item.footprint[k];
      const Position &b = item.footprint[(k + 1) % item.footprint.size()];
      furnitureFace({Position{a.x, base, a.z}, Position{b.x, base, b.z}, Position{b.x, top, b.z}, Position{a.x, top, a.z}});
    }

    ++furnitureComponent;
  }

  for(const auto &slab:plan.siteAccess.slabs)
  {
    if(slab.polygon.size()<3) continue;
    const auto face=[&](std::vector<Position> outline){
      StructureFace f;f.kind=slab.landing || slab.tread ? FaceKind::Landing : FaceKind::Foundation;
      f.material=Material::Stone;f.object=0;f.outline=std::move(outline);f.siteSupport=true;
      out.faces.push_back(std::move(f));
    };
    // Foundations stop beneath the finish layer. Their former top face was
    // coincident with courtyard paving and ground-floor boards. Keep exposed
    // stair treads and landings at their planned walking elevation.
    const double supportTop=slab.landing || slab.tread ? slab.top :
      slab.top-std::min(.02,std::max(0.,slab.top-slab.bottom)*.4);
    auto top=slab.polygon;for(auto &p:top)p.y=supportTop;face(top);
    out.faces.back().pavedSupportTop=true;
    for(size_t k=0;k<slab.polygon.size();++k){const auto a=slab.polygon[k],b=slab.polygon[(k+1)%slab.polygon.size()];
      face({{a.x,slab.bottom,a.z},{b.x,slab.bottom,b.z},{b.x,supportTop,b.z},{a.x,supportTop,a.z}});
    }
  }
  planFacadeInstances(out,plan.facade);
  buildRoofFraming(out,plan);
  deriveEdges(out);

  return out;
}

void deriveEdges(Structure &structure)
{
  structure.edges.clear();

  // Every face's every edge, keyed by its two endpoints to the millimetre,
  // in either order. Two faces naming the same pair share the edge.
  struct Key
  {
    int64_t ax, ay, az, bx, by, bz;

    bool operator<(const Key &o) const
    {
      return std::tie(ax, ay, az, bx, by, bz) < std::tie(o.ax, o.ay, o.az, o.bx, o.by, o.bz);
    }
  };

  const auto quantise = [](const double v) { return int64_t(std::llround(v * 1000.)); };

  const auto keyOf = [&](const Position &p, const Position &q) {
    Key k{quantise(p.x), quantise(p.y), quantise(p.z), quantise(q.x), quantise(q.y), quantise(q.z)};
    Key r{k.bx, k.by, k.bz, k.ax, k.ay, k.az};

    return r < k ? r : k;
  };

  std::map<Key, std::vector<uint32_t>> at;

  for (uint32_t f = 0; f < uint32_t(structure.faces.size()); ++f)
  {
    const std::vector<Position> &ring = structure.faces[f].outline;

    for (size_t k = 0; k < ring.size(); ++k)
    {
      at[keyOf(ring[k], ring[(k + 1) % ring.size()])].push_back(f);
    }
  }

  const auto isRoof = [](const FaceKind kind) {
    return kind == FaceKind::Roof || kind == FaceKind::Deck || kind == FaceKind::Cap || kind == FaceKind::DormerRoof;
  };

  const auto isWall = [](const FaceKind kind) {
    return kind == FaceKind::Wall || kind == FaceKind::WindowedWall || kind == FaceKind::DoorWall ||
           kind == FaceKind::DoubleDoorWall || kind == FaceKind::ArchWall || kind == FaceKind::Parapet ||
           kind == FaceKind::Fence || kind == FaceKind::Gable || kind == FaceKind::Pier || kind == FaceKind::DormerWall;
  };

  const auto carries = [](const FaceKind kind) {
    return kind == FaceKind::WindowedWall || kind == FaceKind::DoorWall || kind == FaceKind::DoubleDoorWall ||
           kind == FaceKind::ArchWall;
  };

  const auto topOf = [](const std::vector<Position> &ring) {
    double top = -1e300;

    for (const Position &p : ring)
    {
      top = std::max(top, p.y);
    }

    return top;
  };

  for (const auto &[key, faces] : at)
  {
    StructureEdge edge;
    edge.a = Position{double(key.ax) / 1000., double(key.ay) / 1000., double(key.az) / 1000.};
    edge.b = Position{double(key.bx) / 1000., double(key.by) / 1000., double(key.bz) / 1000.};
    edge.faces[0] = faces[0];
    edge.faces[1] = faces.size() > 1 ? faces[1] : UINT32_MAX;

    const bool level = std::abs(edge.a.y - edge.b.y) < 1e-6;
    const bool upright = std::abs(edge.a.x - edge.b.x) < 1e-6 && std::abs(edge.a.z - edge.b.z) < 1e-6;
    const FaceKind k0 = structure.faces[faces[0]].kind;
    const FaceKind k1 = faces.size() > 1 ? structure.faces[faces[1]].kind : FaceKind::Count;

    // The role, from what the edge is and what it joins. An edge of a lone
    // face is the outline of the structure; two roof faces meeting level at
    // their highest is a ridge; a wall's top level edge under a roof is an
    // eave; two wall faces meeting upright are a corner, unless one of them
    // carries something, in which case the upright edge is its jamb.
    if (faces.size() < 2)
    {
      // A wall's top edge is an eave whether or not a roof face happens to
      // share its millimetre: the roof oversails it.
      edge.role = level && isWall(k0) && std::abs(topOf(structure.faces[faces[0]].outline) - edge.a.y) < 1e-6 &&
                      edge.a.y > 1e-6
                    ? StructureEdgeRole::Eave
                    : StructureEdgeRole::Outline;
    }
    else if (k0 == FaceKind::DormerRoof && k1 == FaceKind::DormerRoof)
    {
      // A dormer has exactly two roof planes and their only shared edge is
      // its ridge. State that topology directly; millimetre edge-key
      // quantisation must not turn a slightly sub-millimetre height residue
      // into an untyped `Other` edge.
      edge.role = StructureEdgeRole::Ridge;
    }
    else if (level && isRoof(k0) && isRoof(k1) &&
             std::abs(topOf(structure.faces[faces[0]].outline) - edge.a.y) < 1e-6 &&
             std::abs(topOf(structure.faces[faces[1]].outline) - edge.a.y) < 1e-6)
    {
      edge.role = StructureEdgeRole::Ridge;
    }
    else if (level && ((isWall(k0) && isRoof(k1)) || (isRoof(k0) && isWall(k1))))
    {
      edge.role = StructureEdgeRole::Eave;
    }
    else if (upright && isWall(k0) && isWall(k1))
    {
      edge.role = carries(k0) != carries(k1) ? StructureEdgeRole::Jamb : StructureEdgeRole::Corner;
    }
    else
    {
      edge.role = StructureEdgeRole::Other;
    }

    structure.edges.push_back(edge);
  }
}

void meshStructure(const Structure &structure, Solid &out)
{
  // The mesh reads PIECES and the CONTACT GRAPH, never the faces alone
  // (P25d): what touches what is the skeleton's to say.
  meshSkeleton(skeletonOf(structure), out);
}

bool vocabularyIsClosed(std::string *why)
{
  for (uint32_t k = 0; k < uint32_t(FaceKind::Count); ++k)
  {
    if (name(FaceKind(k)) == nullptr)
    {
      if (why != nullptr)
      {
        *why = "a face kind has no name: " + std::to_string(k);
      }

      return false;
    }
  }

  for (uint32_t m = 0; m < uint32_t(Material::Count); ++m)
  {
    if (name(Material(m)) == nullptr || thicknessFor(Material(m)) < 0)
    {
      if (why != nullptr)
      {
        *why = "a material has no name or no thickness: " + std::to_string(m);
      }

      return false;
    }
  }

  return true;
}

} // namespace procedural::world::hierarchy
