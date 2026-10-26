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
  Warehouse,

  // A house of business with a household living over it.
  //
  // Not a shop-house with a bigger shop. A shop-house mixes trade and living
  // through the building; a tavern splits them by floor, because the trade is
  // a room full of strangers and the household cannot be in it. The ground
  // floor is the taproom, the kitchen behind it and a cellar store; the floor
  // over it is the keeper's chamber and what the house keeps.
  Tavern
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
  Corridor,

  // The open bay a building is entered through: ground taken out of the
  // ground floor, roofed by the storey above it and walled on the street side
  // by nothing at all.
  //
  // A porch, an undercroft, the recessed front of a shop. It is a room and not
  // a hole in the plan for the same reason the tower is one: something has to
  // own that ground, or the subdivision cuts a chamber across it and the
  // building ends up with posts standing inside a parlour. What makes it an
  // entrance rather than a room is that its outside walls are `WallKind::Open`
  // - there is no wall there, only the posts carrying the floor above - and
  // that every route into the building starts here.
  Entrance,

  // The inside of a tower: one square of plan, repeated up every storey the
  // tower passes through, and usually carrying the stair.
  //
  // A room and not a hole in the plan, because something has to own that
  // ground on every floor. Left unowned, the floor above cuts rooms straight
  // across it and the building has a tower on the outside with no tower
  // inside it - which is the whole failure this exists to prevent.
  Tower
};

// Every kind of room these buildings have, in one list.
//
// Moved here from the programme generator, which is where it grew up and the
// wrong place for it to live: it is the vocabulary a *plan* is written in, and
// the planner that cuts the rooms could not reach it - so a kitchen came out
// of the complex planner labelled "service", and the only place that knew the
// difference was a generator downstream of it.
//
// `RoomUse` above is the coarse class - is this a room you live in, work in,
// store things in - and stays, because most of the code that consumes a plan
// wants exactly that and nothing finer. This is the fine one, and a room
// carries both: the class it belongs to and the room it actually is.
//
// `Count` is the end marker and doubles as "not said". Several tables are
// sized by it, so nothing may be inserted before it.
enum class RoomKind : uint8_t
{
  // Service. The whole point of these plans: an English house of this period
  // is a polite front and a service wing, and the wing is most of the rooms.
  Kitchen,
  Scullery,
  Larder,
  Pantry,
  Dairy,
  Washhouse,
  Cellar,

  // Polite.
  Parlour,

  // The room a cottage lives in, which is not a parlour: the parlour is the
  // front room kept for visitors and the living room is the one with the fire
  // in it. The corpus separates them on thirteen plates, which is what earns
  // this a slot of its own - the rule being a room type is worth a variable
  // when about eight plates carry it.
  LivingRoom,

  DiningRoom,
  DrawingRoom,
  BreakfastRoom,
  Study,
  Library,

  // Circulation. These are rooms, not gaps between rooms - the corpus names
  // them on the plates (hall 115 plates, passage 45, landing 28) and the
  // adjacency graph is supposed to route through them. A plan whose rooms
  // connect to each other directly is a plan you cross a bedroom to walk.
  Hall,
  Passage,
  Landing,
  Staircase,
  Lobby,
  Porch,

  // Private.
  Bedroom,
  DressingRoom,
  Nursery,
  Closet,
  WaterCloset,
  Bathroom,

  // Working.
  Office,
  Shop,
  Workshop,
  Store,
  Yard,

  // Rooms a large house has and a cottage does not. They sit here rather than
  // with the polite rooms because they only ever appear at the top of the
  // scale, and a vocabulary is easier to read when the rare end is together.
  MorningRoom,
  Boudoir,
  Conservatory,
  Veranda,

  // A deck off an upper room, open on its outward side.
  //
  // Not a room that happens to be outside: a balcony is defined by what it is
  // *not* connected to. It is a leaf of the plan's graph with exactly one edge
  // - the room it belongs to - and its front is open air, so nothing may be
  // laid against it on that side. Those two facts are the whole of it, and
  // they are what the optimiser has to be told rather than left to infer.
  Balcony,
  ServantsHall,
  Brewhouse,

  // A public house. The bar is the plan - everything else is arranged so that
  // nobody can enter or leave without passing within sight of it.
  Bar,
  TapRoom,
  ClubRoom,
  CoffeeRoom,
  BilliardRoom,

  // A school, and a dispensary. Neither is a dwelling, and both have one
  // attached, which is why they are here at all.
  SchoolRoom,
  Dispensary,
  WaitingRoom,
  ConsultingRoom,

  // A farmstead's working buildings, which stand round the yard rather than
  // in the house.
  Stable,
  Barn,
  Loft,

  // A church. The nave and the chancel are one room divided by a screen and
  // by who may stand where, which is the whole plan: everything else hangs
  // off that division.
  Nave,
  Chancel,
  Aisle,
  Transept,
  Sanctuary,
  Narthex,
  SideChapel,
  Vestry,
  Crypt,
  Belfry,
  Cloister,
  ChapterHouse,

  // A gallery runs round the inside of a preaching box and round the inside
  // of a prison, for the same reason: it doubles the number of people who can
  // see one point.
  Gallery,

  // Castle and medieval hall. The great hall is not a big hall - it is a
  // different room, open to its roof, with the household eating in it and the
  // service rooms at one end behind a screen.
  GreatHall,
  ScreensPassage,
  Solar,
  Buttery,
  Keep,
  Gatehouse,
  Guardroom,
  Barracks,
  Armoury,
  Dungeon,
  Tower,
  Courtyard,
  Well,

  // A prison.
  Cell,
  Infirmary,
  ExerciseYard,

  // The state rooms of a mansion, which a large house does not have and which
  // are the reason a mansion is a different building rather than a big one.
  Saloon,
  Ballroom,
  SmokingRoom,
  GunRoom,

  // A mansion's service establishment, which is a household of its own.
  StillRoom,
  HousekeepersRoom,
  ButlersRoom,

  // Trade. A counting house is where the books are kept and the merchant sits;
  // a strong room is where the money is; an undercroft is the vaulted storey
  // under a hall, which a guild lets out as shops and a merchant uses himself.
  CountingHouse,
  StrongRoom,
  CouncilChamber,
  Undercroft,

  // The medieval countryside, which is most of the buildings there are.
  //
  // A longhouse is one range under one roof with the family at one end and the
  // beasts at the other, a cross passage between them, and the byre floor laid
  // to fall away from the living end. It is not a cottage with a shed: the
  // animals are in the building and their warmth is part of how it is heated.
  Byre,
  Croft,   // The worked ground behind the house: vegetables, not lawn.
  Garden,  // ...and the ground kept for pleasure, which is a wealthy idea.
  Orchard,
  Pen,     // Hurdled ground for stock, against the yard.
  Midden,

  // Housing people in numbers: an almshouse, a guild's lodging for its
  // journeymen, a mill's row, a barrack of farm servants. One room of beds and
  // a room to eat in, repeated.
  Dormitory,
  Refectory,
  // Added for the cell planner: the six purposes Watabou's vocabulary has that
  // ours did not. All appended, never inserted - `Count` is the end marker and
  // several tables are sized by it, so a value placed anywhere but here
  // re-maps every one after it and renders as no change at all.
  Observatory,  // A room for looking at the sky. Top floor, few nooks.
  Laboratory,   // An alchemist's or apothecary's work room, which is not a workshop.
  Studio,       // An artist's room: it wants light and floor, not a bench.
  TrophyRoom,   // For displaying what was hunted or taken.
  Lookout,      // A room whose point is the view: half its wall is outside wall.
  Attic,        // The sole room of a top floor, under the roof - not a hay loft.


  Forge,
  Bakehouse,
  Granary,
  Count
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

  // How many households live here.
  //
  // One is a house. Two is a building with a dwelling to a floor, and the upper
  // one is not reached through the lower - there is no stair between them at
  // all. It is reached from outside, by a flight up to a gallery, and its front
  // door is on that gallery.
  //
  // A field and not a use, because it cuts across every use: a shop with a
  // household over it is one dwelling with a trade in it, and the same building
  // let to two families is two dwellings with no connection. The plan is what
  // differs and the outside barely does.
  uint32_t households = 1;

  // What stands against this building: the ground plan of each neighbour near
  // enough to matter, counter clockwise, in the same frame as `shell`.
  //
  // Only windows read it, and only to refuse: a wall with another building a
  // stride away is not an outside wall in any sense a window cares about. The
  // planner cannot work this out for itself - a building knows its own plot
  // and nothing beyond it - so a caller that knows the street fills this in,
  // and one that does not leaves it empty and gets a house standing alone.
  std::vector<std::vector<Position>> neighbours;

  // The way in from outside is somebody else's.
  //
  // A tenement standing alone has to build its own gallery and its own flight
  // up to it, because otherwise the upper household is sealed in - so the
  // planner forces both, whatever the settings say. In a neighbourhood that is
  // one gallery too many: the gallery outside this building's first floor is
  // shared with the building next door, it is built by the layer that knows
  // there is a building next door, and a second one against the same wall is
  // not a second way in, it is a fault.
  //
  // It does not loosen the rest of the arrangement. There is still no stair
  // between two households and each floor is still a whole dwelling; the only
  // thing this says is that the outside of it is not this building's to build.
  bool externalAccess = false;

  // The top is a floor rather than a roof.
  //
  // A pitched roof is what a building has when nothing stands on it. Where
  // something does - a neighbour's gallery, a stair's head, the storey nobody
  // has the money for yet - the top is a slab with a rail round it, which is
  // the same thing the open-topped tower already builds and is most of why a
  // hillside of these reads as a favela rather than as a village.
  //
  // Last in the struct on purpose: this brief is initialised positionally in
  // places, and a field inserted above the last silently re-maps every value
  // after it.
  bool flatTop = false;
};

struct DwellingRoom
{
  uint64_t id = 0;
  uint32_t floor = 0;
  RoomUse use = RoomUse::Living;
  std::vector<Position> boundary;
  double usableArea = 0;

  // How many storey heights stand between this room's floor and its ceiling.
  //
  // A hall is the one room in a medieval building that is not one storey
  // tall, and every account of one - a guild hall, a church nave, the hall of
  // a manor - gives its height before it gives its plan. One is an ordinary
  // room; two is a hall open through the floor that would otherwise be over
  // it, and that floor then has no rooms there at all.
  uint32_t heightFloors = 1;

  // ...and where that ceiling is, absolute, in metres. Zero means the storey's
  // own height, unchanged.
  double ceilingHeight = 0;

  // No ceiling at all: what you see looking up is the roof over the building.
  bool openToRoof = false;

  // What the room actually is, as against what class it belongs to.
  //
  // `use` says "service" and this says "scullery". Last in the struct because
  // rooms are built with positional initialisers in places, and `Count` when
  // whoever built it did not say.
  RoomKind kind = RoomKind::Count;
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

  // Horizontal unit vector from the item's centre towards the wall it faces.
  // Kept in the blueprint so a structural/detail pass can tell a bed's head
  // from its foot or which side of a counter serves the room.
  Position facing{};
};

// What actually stands on a wall's line.
//
// A plan drawn as closed rings says every boundary between two spaces is a
// wall, and most of them are. The ones that are not are the ones that make a
// building read as a building: the open front of an entrance bay, where the
// storey above is carried on posts and you can walk straight through; the
// broad opening of a shop, which is a wall for the height of a counter and
// shuttered air above it. Both are boundaries in the plan and neither is
// masonry, so the mesher must be told which - it cannot be read back off the
// geometry, because the geometry is the same ring either way.
enum class WallKind : uint8_t
{
  // Masonry or frame, floor to ceiling. Every wall that is not stated
  // otherwise.
  Solid,

  // No wall: the boundary exists in the plan and nothing is built on it. The
  // open side of an entrance bay, carried on the pillars beside it.
  Open,

  // A shop front: solid to the height of a counter, open above it. What a
  // merchant's ground floor presents to the street.
  Shopfront,

  // A rail, not a wall: the edge of a floor that stops before the room does.
  //
  // Where a hall is two storeys tall, the storey above it has a hole in it,
  // and the boundary between the floor and the hole is a balustrade about a
  // metre high with posts in it - you look down into the hall over it. It is a
  // wall in the plan, because it is a boundary between two rooms and doors and
  // windows have to know not to go in it; it is not a wall in the building.
  //
  // New kinds go last: `DwellingWall` is built with positional aggregates in
  // several places and a value inserted above the last re-maps every one.
  Rail
};

const char *wallKindName(WallKind);

struct DwellingWall
{
  uint64_t id = 0;
  uint32_t floor = 0;
  Position a, b;
  double thickness = .2;
  bool exterior = false;
  bool party = false; // Authored shared-lot wall, preserved through structure and voxel assembly.

  // What stands here. See WallKind: an open wall is a boundary with nothing
  // built on it, and the mesher must not extrude one.
  WallKind kind = WallKind::Solid;

  // How tall the wall stands, in metres. Zero takes the storey's own height,
  // which is what every wall but a hall's does.
  double height = 0;
};

struct DwellingDoor
{
  uint64_t wall = 0, room = 0;
  uint32_t floor = 0;
  Position center;
  double width = .9;
  bool privateAccess = false, exterior = false;
  bool shopOpening = false; // Broad selling opening on a shop front, not an access door.

  // The room on the other side of it, or zero for the way in from outside.
  //
  // A door was a point and a destination, which is enough to draw one and not
  // enough to cut one. Two rooms both emit the wall they share, so the hole
  // has to be cut twice - and the only way to know which two walls those are
  // is to know which two rooms the door joins. Without it the rule has to be
  // geometric ("any wall this point lies on"), and a long straight run of plan
  // puts several rooms' walls on one line: an inner door then cut a hole in
  // the outside wall of a room three doors away, which is a doorway in the
  // side of the building opening onto a drop.
  //
  // Last in the struct, and that is not tidiness. Doors are built here and
  // there with positional initialisers - `{wall, room, floor, centre, width,
  // private, exterior}` - and a field inserted above the last of them silently
  // re-maps every value after it. Put in the middle, this one turned every
  // door in the legacy recipe into a door somewhere else.
  uint64_t fromRoom = 0;
  uint8_t leaves = 1; // Preserve the blueprint choice even on narrow grid bays.
};

// What a hole in a wall is for.
enum class OpeningKind : uint8_t
{
  Door,       // Walked through: the cut reaches the floor.
  Window,     // Looked through: the cut starts at a sill.
  ShopWindow  // A merchant's display opening: wider and lower than a window.
};

// A rectangular void cut clean through a wall, for a door or window mesh to be
// set into later.
//
// Not the door. A door is a leaf on hinges with a frame around it and a
// threshold under it, and none of that is the wall's business - the wall's
// business is the hole. So this is the hole and nothing else: a run along the
// wall, a sill and a head, and the wall's full thickness. It is cut deliberately
// larger than the joinery that will fill it, because a frame needs somewhere to
// sit and a reveal you have to model around is worse than a reveal you were
// given; `DwellingOpening` is the void, and the mesh that fills it carries its
// own margin.
//
// `a` and `b` are the ends of the run on the wall's centre line, at the storey's
// floor level. Together with `thickness` they give the box: sweep the segment
// through the wall's thickness and up from `base` to `top`.
//
// The void is the full height of the wall, floor to ceiling, and wider than the
// joinery by a margin at each end. That is not an approximation of a window
// opening - it is a deliberate over-cut, so whoever models the window or the
// door is given a clear rectangular hole and can put whatever they like in it:
// the frame, the panel under the sill, the light over the lintel, the bracing
// of a timber frame. A tight hole would decide all of that here, in a
// generator that knows nothing about how the building is drawn.
//
// `sill` and `head` say where the glazing or the leaf is meant to sit inside
// the void. They are a hint for whoever fills it, not the cut.
struct DwellingOpening
{
  uint64_t wall = 0, room = 0;
  uint32_t floor = 0;
  Position a, b;               // The run along the wall, at floor level.
  double base = 0, top = 2.6;  // The cut itself, above the storey's floor.
  double sill = 0, head = 2.1; // Where the joinery goes inside it.
  double thickness = .2;       // The wall's, so the cut goes all the way through.
  OpeningKind kind = OpeningKind::Door;
  bool exterior = false;
};

const char *openingKindName(OpeningKind);

// The name of a room kind, for diagnostics and for legends.
const char *roomKindName(RoomKind);

// What a piece of the timber frame is for.
//
// Not a material and not a size: the size follows from the job. A frame of this
// age is a small number of large members carrying a large number of small ones,
// and that ratio is most of what makes a floor read as a structure rather than
// as a grid of identical sticks.
enum class TimberKind : uint8_t
{
  // Along the top of a wall, where the floor above bears on it.
  Plate,

  // The big members: one span across the building at a few metres' centres. A
  // bridging beam, or a summer beam where it is the only one.
  Beam,

  // The small ones, at close centres, running the other way over the beams.
  // What the ceiling is fixed to.
  Joist,

  // Vertical, under a beam end that lands on no wall.
  Post,

  // In the roof, following its pitch rather than the plan's axes.
  Rafter,

  // The diagonal that makes a corner of the frame a triangle.
  Brace,

  // Outside, and the only timber that is: the uprights of the wall frame,
  // between which the panels are filled in.
  Stud,

  // ...and the horizontal members they are framed between.
  Rail
};

const char *timberKindName(TimberKind);

// One piece of timber: a line and a section.
//
// Deliberately not a box. Where a timber runs and how big it is across are the
// two facts a later pass needs, and every finished shape - a rotated
// rectangular prism, a carved instance, a member with a joint cut in each end -
// can be built from them. Emitted as a solid here, that pass would have to
// recover the line from the geometry first.
//
// `a` and `b` are the ends of the centre line, in metres, in the world frame.
// `width` is across the member and `depth` through it, about the line.
struct DwellingTimber
{
  uint32_t floor = 0;
  TimberKind kind = TimberKind::Joist;
  Position a, b;
  double width = .08, depth = .12;
};

struct DwellingStair
{
  std::vector<Position> opening, lowerLanding, upperLanding;
  uint32_t lowerFloor = 0, risers = 0;
  double rise = 0, tread = .27, width = 1.1;
  double startRise = 0; // Height above the lower floor where this flight starts (second flight of a turning stair).
  bool privateAccess = false;
  bool wallSupported = false; // Thin flights and landings fixed to the shaft walls.

  // A flight that stands in a SHAFT rather than on a storey's floor: the
  // turret's climb, which winds round the inside of its own wall and passes
  // every floor without belonging to one. It is checked against the shaft it
  // is in, not against the floor plan it is outside of.
  bool external = false;
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

  // How far the ridge rises per metre across the mass, when this wing leads
  // its cluster. Zero takes the default pitch.
  //
  // A pitch is not a property of a roof's shape, it is a decision about the
  // building under it: a barn sheds at a third, a hall or a church carries
  // its height in the roof and stands at two thirds, and a tower's spire is
  // steeper than either.
  double roofPitch = 0;

  // This wing is a tower: it rises past the rest of the building and is
  // roofed on its own.
  bool tower = false;

  // No roof was built over this wing's mass, and why.
  //
  // The roof is a medial axis, exact only where the reflex corners of a plan
  // are square, and it refuses a plan rather than draw ridges in the wrong
  // place over one that is oblique. That refusal used to reach nobody but
  // stderr, so a building standing bare was indistinguishable from a building
  // the roof had never been asked about. It is part of the model now: whatever
  // consumes a plan can tell the two apart, and so can a test.
  bool roofRefused = false;
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
  bool radial = false; // Tower cap: facets approximate one roof, not separate hips.
};

// A small roofed projection through a main roof.
//
// Kept as a first-class blueprint object rather than baked into the roof mesh:
// the structural pass needs to know which shell faces form one dormer, which
// main roof carries it, and which face is glazing. A later detail pass can then
// replace this deliberately simple shell with cheeks, flashing, frames and
// tiles without rediscovering any of that from triangles.
struct DwellingDormer
{
  uint64_t facadeWall = 0; // Rectilinear exterior segment the front aligns with.
  // Faces left around the aperture after the carrying roof has been cut. A
  // dormer is supported by the boundary, not by an unbroken polygon hidden
  // underneath it.
  std::vector<uint32_t> roofs;
  std::vector<std::vector<Position>> walls;
  std::vector<std::vector<Position>> roofFaces;
  std::vector<Position> window;
  std::vector<Position> opening; // Aperture cut through the carrying roof.
  std::vector<Position> ceiling; // Interior ceiling below the little roof.
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

  // A mass rather than a post: a stair turret, a chimney stack, a buttress.
  //
  // Appended at the end, and it earns its place: a post carrying a balcony and
  // a tower carrying a staircase are the same prism and are not the same
  // thing, and anything drawing them wants the tower in masonry and the post in
  // timber. Without it the turret came out the colour of a chimney.
  bool masonry = false;
  bool capped = false;

  // Stands INSIDE the building, carrying the floor above.
  //
  // A post under a beam and a post under a balcony are the same prism, the same
  // timber and the same height, and the only thing that tells them apart is
  // which side of the wall they are on. Said here rather than inferred: a check
  // that a balcony's posts stand on open ground read every post in the building
  // as a balcony's the moment rooms started carrying their own, and reported
  // the arcade of a hall as a balcony hanging over its own floor.
  //
  // Last in the struct, like `masonry` above it and for the same reason.
  bool interior = false;
};

struct DwellingFloor
{
  double elevation = 0, height = 2.8, grossArea = 0, roomArea = 0, circulationArea = 0, coreArea = 0, wallArea = 0;
  std::vector<std::vector<Position>> circulation, slabs;

  // Ground that is a room and not a floor: a yard, a court. Paved, at grade,
  // with nothing over it. Its own list because it is drawn as ground rather
  // than as boards, and last in the struct for the usual reason.
  std::vector<std::vector<Position>> paving;
};

// A flat piece of a plan, as a polygon rather than as triangles.
//
// The order the geometry is settled in: the planner decides a face, and
// whatever draws or meshes it triangulates that face. Deciding triangles
// directly is how a surface and its own outline end up disagreeing.
struct DwellingFace
{
  std::vector<Position> outer;              // Counter clockwise.
  std::vector<std::vector<Position>> holes; // Clockwise, strictly inside.
  double height = 0;
  uint32_t floor = 0;
};

enum class EntranceLayout : uint8_t { Ground, Inset, Sideways };
struct DwellingSiteSlab
{
  std::vector<Position> polygon;
  double bottom=0, top=0;
  bool landing=false, tread=false;
};
struct DwellingSiteAccess
{
  EntranceLayout layout=EntranceLayout::Ground;
  double rise=0;
  std::vector<uint32_t> reservedCells;
  std::vector<DwellingSiteSlab> slabs;
};
enum class DwellingArchitecture : uint8_t { Vernacular, Roman, HighFantasy };
inline const char *dwellingArchitectureName(DwellingArchitecture s) {
  return s==DwellingArchitecture::Roman?"Roman-inspired":s==DwellingArchitecture::HighFantasy?"High fantasy":"Vernacular";
}
enum class DoorProfile : uint8_t { Automatic, Rectangular, RoundArch, PointedArch };

enum class FacadeStyle : uint8_t { Domestic, Romanesque, Gothic };
inline constexpr double kPassageArchSpring = .92;

struct DwellingFacadePolicy
{
  bool enabled = false;
  // Cell boundaries are the exterior envelope, not wall centre lines.
  bool inwardWalls = false;
  bool roofFraming = false;
  bool roofCrests = false; // Bounded ridge ornaments on actual shared roof edges.
  bool corridorSupports = false;
  bool timberFacade = false;

  // Whether the frame is built as BEAMS as well as being in the wall's
  // texture. Off: the members were a post, a plate and a brace per panel and
  // a tie, a king post and two rakers per gable - tens of thousands of boxes
  // a city, which read as a scribble of dark lines rather than as timber. The
  // half-timber material carries the pattern instead.
  bool timberFrameMembers = false;

  // How far apart the posts of a timber frame stand. A bay, not a cell: the
  // cell model emits a wall face per lattice edge, so framing each face gave a
  // post every metre on a city house - four hundred members on one cottage,
  // and a facade that read as a brown palisade instead of a frame.
  double timberBayMetres = 1.8;
  double stoneBaseHeight = 0;
  double windowWidth = 1.0, windowHeight = 1.0;
  bool boardFacade = false; // Plain timber boarding for modest wooden huts.
  bool stoneVaults = false;
  bool castleInterior = false;
  FacadeStyle style = FacadeStyle::Domestic;
  DoorProfile doorProfile = DoorProfile::Automatic;
  double bayMetres = 2.8;
  bool glazed = true;
  bool shutters = false;
  bool circularWindows = false;
  bool masonryVariation = false;
  uint64_t variationSeed = 0;
  int32_t maximumWindows = -1;
  // Explicit open galleries/porticos only: never remove an occupied room's
  // enclosure simply because its architectural style permits columns.
  std::vector<uint64_t> arcadeWalls;
  std::vector<uint64_t> passageWalls;
};

struct DwellingCoveredPassage
{
  uint64_t id=0,room=0;
  std::vector<Position> polygon;
  std::vector<uint32_t> cells;
  std::vector<uint64_t> portalWalls;
  double clearance=0;
  uint32_t occupiedFloor=1;
};

// A wall-mounted chamber, reached directly from an occupied upper floor.
// Geometry is emitted as ordinary rooms, walls, slabs and roofs; this record
// retains the attachment and support decisions for validation and editing.
struct DwellingWallTurret
{
  uint64_t room = 0, hostWall = 0;
  uint32_t floor = 0;
  bool round = false;
  double base = 0, top = 0, corbelBase = 0;
  std::vector<Position> footprint;
};

struct DwellingPlan
{
  uint64_t id = 0, seed = 0;
  DwellingUse use = DwellingUse::Cottage;

  std::vector<DwellingRoom> rooms;
  std::vector<DwellingWall> walls;
  std::vector<DwellingDoor> doors;
  std::vector<DwellingOpening> openings;
  std::vector<DwellingStair> stairs;

  // The frame that carries the floors and the ceilings. Part of the interior:
  // a street sees none of it.
  std::vector<DwellingTimber> timbers;
  std::vector<DwellingFurniture> furniture;
  std::vector<DwellingWing> wings;
  std::vector<DwellingRoofFace> roofs;
  std::vector<DwellingDormer> dormers;
  std::vector<DwellingStorey> storeys;
  std::vector<DwellingPillar> pillars;
  std::vector<DwellingWallTurret> wallTurrets;

  // The chimneys: masonry stacks standing on a hearth and passing through the
  // roof. The same shape as a pillar - a footprint and two heights - and a
  // different thing, so it is a different list.
  std::vector<DwellingPillar> chimneys;
  std::vector<DwellingFloor> floors;
  uint32_t residentCapacity = 0, workerCapacity = 0;
  bool fallback = false; // The legacy two-band recipe was used after the planner declined.
  std::string note;      // Why the planner declined, when it did.

  // The ceiling: what you see looking up from the top storey of each wing.
  //
  // Triangles, at the wall head, covering the ground that has no storey over
  // it - so a two storey range and the single storey wing beside it each get
  // their own, at their own heights. Below that line a storey's ceiling is the
  // boards of the storey above and needs nothing here.
  //
  // Holes rather than surface: the tower shaft, the chimney and the void over
  // a hall open to its roof are all places where you look up and do *not* see
  // a ceiling, and a plane laid across any of them is a lid over the thing it
  // is supposed to leave open.
  //
  std::vector<std::vector<Position>> ceilings;

  // ...and the same ceiling before anything cut it into triangles.
  //
  // A face is what the planner actually decides: one polygon, at one height,
  // with the shaft and the flue and the void over a hall taken out of it as
  // holes. The triangles above are a consequence of that face and not a
  // separate description of it - which matters because the two can disagree,
  // and when they do it is always the triangulation that quietly filled a hole
  // in.
  //
  // Both are carried so nothing that already reads the triangles has to
  // change, and so a test can hold one against the other: the faces say what
  // the ceiling is, the triangles say what will be drawn, and their areas have
  // to match.
  //
  // Last in the struct on purpose - plans are built with positional
  // initialisers in places, and a field inserted above the last silently
  // re-maps every value after it.
  std::vector<DwellingFace> ceilingFaces;
  DwellingSiteAccess siteAccess;
  DwellingFacadePolicy facade;
  std::vector<DwellingCoveredPassage> coveredPassages;

  // The lattice this plan was laid out on, in metres, or zero when it was not
  // laid out on one at all.
  //
  // A cell dwelling's walls are one per lattice edge and its rooms are whole
  // cells, so the cell is the plan's own module - and the structure needs it
  // to keep that module. Without it the structure re-cut every room side into
  // bays of its own choosing and the modularity was lost between the plan and
  // the mesh: measured, 19% of wall faces were a whole cell wide.
  //
  // Last, with the rest: see `ceilingFaces` above on positional initialisers.
  double cellMetres = 0;
  DwellingArchitecture architecture= DwellingArchitecture::Vernacular;
  uint32_t architecturalAgeYears=0;
  double weathering=0;
  uint32_t colonnadeColumns=0,cofferBeams=0;
  // Ordered rear-left, rear-right, front-right, front-left; planned with the
  // portico roof and columns. The city may grade heights, never invent its footprint.
  std::vector<Position> entranceLanding, entranceFlight;

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
