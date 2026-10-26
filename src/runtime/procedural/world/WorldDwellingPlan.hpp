#pragma once
#include "WorldGenerationContracts.hpp"
#include <functional>
#include <string>
#include <vector>

namespace procedural::world::rewrite {
enum class DwellingUse
{
  Cottage,
  ShopHouse,
  Lodging,
  Hall,
  Warehouse
};
enum class RoomUse
{
  Living,
  Sleeping,
  Shop,
  Store,
  Common,
  Service,
  Hall,

  // Circulation as a space of its own.
  //
  // A corridor is a room with a purpose, not the absence of one. Without it,
  // rooms are reached through other rooms - you walk through a bedroom to get
  // to a bedroom - and doors end up on whatever wall two rooms happen to
  // share. Watabou's Dwellings generates corridors freely enough that the
  // author's own complaint about it is that there are sometimes too many.
  Corridor
};

struct DwellingBrief
{
  uint64_t id = 0, seed = 1;
  DwellingUse use = DwellingUse::Cottage;
  std::vector<Position> shell; // Accepted rectangular wing, CCW, metres.
  Position entrance;           // Fixed on ground-floor exterior wall.
  double foundation = 0, floorRise = 2.8, availableHeight = 6, wallThickness = .2;
  uint32_t floors = 1;
  // Jettying (A1): accepted street-facing projection of each floor beyond the
  // floor below, metres, indexed by floor (ground floor entry is ignored).
  // Empty means every floor is flush. The ground footprint never changes.
  std::vector<double> jettyMetres;
};

struct DwellingRoom
{
  uint64_t id = 0;
  uint32_t floor = 0;
  RoomUse use = RoomUse::Living;
  std::vector<Position> boundary;
  double usableArea = 0;
};

// What a room is furnished with: abstract shapes, by purpose.
//
// Watabou's Dwellings draws furniture as plain rectangles and it is most of
// what makes one of his plans readable at a glance - a room with a bed in it
// needs no legend to say it is a bedroom. The shapes are not a simulation of
// furnishing: they are the smallest thing that says what a room is for.
enum class FurnitureKind : uint8_t
{
  Bed,
  Table,
  Counter,  // A shop's serving counter, against the street wall.
  Hearth,   // Against a wall, in the room a household lives in.
  Chest,    // Storage.
  Bench,    // Seating along a wall in a common room.
  Workbench // A craftsman's bench, in a service room.
};

const char *furnitureKindName(FurnitureKind);

struct DwellingFurniture
{
  uint64_t room = 0;
  uint32_t floor = 0;
  FurnitureKind kind = FurnitureKind::Table;
  std::vector<Position> footprint; // Rectangle, counter clockwise, metres.
};

struct DwellingWall
{
  uint64_t id = 0;
  uint32_t floor = 0;
  Position a, b;
  double thickness = .2;
  bool exterior = false;
};

struct DwellingDoor
{
  uint64_t wall = 0, room = 0;
  uint32_t floor = 0;
  Position center;
  double width = .9;
  bool privateAccess = false, exterior = false;
  bool shopOpening = false; // Broad selling opening on a shop front, not an access door.
};

struct DwellingStair
{
  std::vector<Position> opening, lowerLanding, upperLanding;
  uint32_t lowerFloor = 0, risers = 0;
  double rise = 0, tread = .27, width = 1.1;
  double startRise = 0; // Height above the lower floor where this flight starts (second flight of a turning stair).
  bool privateAccess = false;
};

// One wing of the exterior, and the roof that covers it.
//
// "A mansion consists of 'wings' - rectangular blocks, each with its own roof",
// and "wings are grouped into 'clusters' - sets of wings whose roofs can be
// merged organically". That is the mansion generator's own account of its
// outside, and it is why "there is a one-one match between its exterior and
// floor plan": the wings are the plan, seen from above.
//
// A wing is therefore not decoration on top of a finished interior. It is the
// same decomposition the interior is planned on, read at the scale of the
// building rather than the room, and it carries the storey count that part of
// the plan supports.
struct DwellingWing
{
  std::vector<Position> footprint; // The wing's own ground, counter clockwise.
  Position ridgeFrom, ridgeTo;     // The roof ridge, along the wing's long axis.
  double eavesHeight = 0;          // Where the roof starts.
  double ridgeHeight = 0;          // ...and its peak.
  uint32_t storeys = 1;
  uint32_t cluster = 0; // Wings sharing one are roofed as a single mass.
};

// One face of a wing's roof.
//
// A wing carried only a ridge segment, which is a line and not a surface: it
// says where the roof peaks and nothing about what covers the building. So the
// exterior could be drawn as a plan and never as a building, and every
// question about it - does the roof reach the walls, do two wings meet or
// interpenetrate, is that end a gable or a hip - had no answer in the data.
//
// These are the surfaces. Each one runs from an eaves edge up to the ridge, so
// the roof of a rectangular wing is two trapezoids and two ends, and the ends
// are triangles sloping inward (a hip) or vertical (a gable).
struct DwellingRoofFace
{
  std::vector<Position> polygon;
  uint32_t wing = 0;
  bool gable = false;  // Vertical: an end wall carried up to the ridge.
  bool soffit = false; // The underside of the overhang, closing roof back to wall.
};

// One storey of the exterior: the ground the building covers at that level.
//
// A wing was drawn as a prism from the ground to its own eaves, and the walls
// came from each wing's own outline. Two things follow from that and both are
// wrong. Where two wings meet, each emitted the shared wall, so the model
// carried a duplicate face inside the building. And a wing had one shape from
// the ground to the roof, so a building could never step back as it rose -
// which is the whole of how a mansion reads as a mansion.
//
// "The number of storeys at any particular place of a building is defined by
// its ground plan." So the exterior is a stack of plates: the plate at level k
// is the union of the ground of every wing that carries more than k storeys.
// The ground floor is the whole plan, the floor above it is only the parts
// tall enough to have one, and the outline changes as it goes up. The walls of
// a level are that plate's boundary, so a shared wall is interior to the
// union and is never emitted at all.
struct DwellingStorey
{
  std::vector<Position> outer;
  std::vector<std::vector<Position>> holes;
  uint32_t level = 0;
  double base = 0, top = 0;
};

// A post carrying an upper storey over open ground.
//
// What stands under a jettied or arcaded front: the ground floor is set back
// and the floor above it is not, so something has to hold the overhang up.
struct DwellingPillar
{
  std::vector<Position> footprint;
  double base = 0, top = 0;
};

struct DwellingFloor
{
  double elevation = 0, height = 2.8, grossArea = 0, roomArea = 0, circulationArea = 0, coreArea = 0, wallArea = 0;
  std::vector<std::vector<Position>> circulation, slabs;
};

struct DwellingPlan
{
  uint64_t id = 0, seed = 0;
  DwellingUse use = DwellingUse::Cottage;
  std::vector<DwellingRoom> rooms;
  std::vector<DwellingWall> walls;
  std::vector<DwellingDoor> doors;
  std::vector<DwellingStair> stairs;
  std::vector<DwellingFurniture> furniture;
  std::vector<DwellingWing> wings;
  std::vector<DwellingRoofFace> roofs;
  std::vector<DwellingStorey> storeys;
  std::vector<DwellingPillar> pillars;
  std::vector<DwellingFloor> floors;
  uint32_t residentCapacity = 0, workerCapacity = 0;
  bool fallback = false; // The legacy two-band recipe was used after the planner declined.
  std::string note;      // Why the planner declined, when it did.
};

struct DwellingTriangle
{
  Position a, b, c;
  uint32_t material = 0;
};

// Bounded rectangular-wing qualification. Voronoi cells within room bands use
// the same MapPartition implementation as the world map. Unsupported shells,
// fixed doors, programmes or stairs fail atomically; no forced tiny rooms.
bool generateDwellingPlan(const DwellingBrief &, DwellingPlan &, std::string *error = nullptr);
bool meshDwellingPlan(const DwellingPlan &, std::vector<DwellingTriangle> &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
