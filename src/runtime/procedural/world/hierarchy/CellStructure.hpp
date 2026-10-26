#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/hierarchy/Generator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::hierarchy {

// The structural view: what the blueprint BECOMES before anything is drawn.
//
// Three processes in a line - Blueprint Generation, Structural Generation, 3D
// Mesh Generation - and this is the middle one's output. The plan says where
// walls are, what is cut in them and how high the roof rises; the mesh step
// wants vertices. Between them stood one function that turned the plan into
// faces directly, with the geometry baked in at the point the plan was read: a
// window was a hole and a pane, a pier was four quads, and nothing in between
// said WHAT any face was. This is that in-between: typed faces, each with a
// material and the object it belongs to, and nothing about how it is drawn.
//
// Two rules that follow from the request that shaped it:
//
// A wall is two sides. The outside of the building is one object and every
// room is another, and a wall between them is two faces - one belonging to
// each - a thickness apart. The inside is generated as its own object, not
// derived from the outside afterward, and the two agree about the wall
// because both were cut from the same segment.
//
// A window is a property of the wall, not a cut in it. A wall is divided
// along its length into segments, after the roof, and a segment either is
// plain or carries exactly one thing - a window, a door, a pair of doors, an
// arch - and that is its kind. No holes, no panes, no reveals here; the mesh
// step draws a windowed wall as its rectangle with a darker square in it, and
// a later mesh step can put real joinery there without this changing.
enum class FaceKind : uint8_t
{
  Wall,           // A plain segment of wall.
  WindowedWall,   // A segment carrying one window.
  DoorWall,       // ...one door.
  DoubleDoorWall, // ...a pair of doors.
  ArchWall,       // ...an open arch.
  Parapet,        // Wall above a deck, waist high or so.
  Fence,          // A yard's wall: low, its own thing.
  Rail,           // An open balustrade.
  Roof,           // A sloping roof surface.
  Gable,          // The vertical triangle closing a roof against a taller wall.
  Soffit,         // The underside of an overhang.
  Deck,           // A flat roof or a gallery: walked on, open above.
  Cap,            // A cone or pyramid over a tower.
  Pier,           // A masonry pier of a lantern or arcade.
  Post,           // A timber post.
  Floor,          // Boards you stand on, inside.
  Ceiling,        // Downward-facing room enclosure, with circulation voids.
  Paving,         // Ground that is a room: a yard, a court.
  Ramp,           // One flight: an inclined rectangle from its foot to its head.
  Landing,        // A level pad a flight starts from, turns on or arrives at.
  Timber,         // A member of the frame or a ladder: a rung, a stile.
  Furniture,      // One face of a purpose-carrying furniture box.
  DormerOpening,  // The aperture cut from the carrying roof (not a surface).
  DormerWall,     // The front and cheeks of a roof projection.
  DormerWindow,   // Glazing in the dormer's front.
  DormerRoof,     // One of the projection's small tiled roof faces.
  DormerCeiling,  // Interior horizontal closure below the dormer's roof.
  Foundation,    // Structural support beneath the selected building cells.
  Count
};

const char *name(FaceKind);

// What a face is made of. The thickness of a wall follows from this and not
// from the wall: stone is thicker than timber, and a type that carried its
// own thickness could disagree with its material.
enum class Material : uint8_t
{
  Stone,
  Timber,
  Wattle,
  Tile,
  Boards,
  Earth,
  Count
};

const char *name(Material);

// The wall thickness a material builds to, in metres.
double thicknessFor(Material);

enum class ApertureShape : uint8_t { Rectangle, RoundArch, PointedArch, Circle, SegmentalArch };
enum class FacadeAsset : uint8_t { Frame, Mullion, Glass, DoorLeaf, Column, ColumnLeftHalf, ColumnRightHalf, PortalFrame, DoorLeafLeft, DoorLeafRight, Shutter, ArcadeArch, WindowSill, WallBlock };
enum class FacadeEdge : uint8_t { Plain, PillarLeftHalf, PillarRightHalf };
enum class TimberRole : uint8_t { Other, RoofEdge, Ridge, Rafter, Purlin, TieBeam, CeilingJoist, KingPost };

// A unit aperture is shared by both wall skins and its atlas frame. The mesh
// cannot select a different shape or size from the one the structure cut.
struct FacadeInstance
{
  FacadeAsset asset = FacadeAsset::Frame;
  ApertureShape shape = ApertureShape::Rectangle;
  double archSpring = -1; // Optional normalized spring shared with the wall opening.
  rewrite::Position origin{}, u{1,0,0}, v{0,1,0}, normal{0,0,1};
  double width = 1, height = 1, depth = .08;
  uint32_t segment = UINT32_MAX, supportFace = UINT32_MAX;
  bool inside = false;
  bool wooden = false;
};

// Which side of a face the object it belongs to is on: a wall segment between
// the outside and a room is emitted twice, once facing out for the exterior
// object and once facing in for the room, a thickness apart.
struct StructureFace
{
  bool masonryVault = false;
  bool vaultTransition = false;
  bool pavedSupportTop = false;
  bool roofJointCap = false;
  bool radialRoof = false;
  bool engagedArch = false;
  double timberInfillArea = 0; // Wall-plane area occupied by posts/plates.
  FaceKind kind = FaceKind::Wall;
  Material material = Material::Stone;
  uint32_t object = 0; // Index into Structure::objects.
  bool party = false; // A shared-lot wall skin; instance boundary policy is resolved city-wide.
  std::vector<rewrite::Position> outline; // Counter-clockwise seen from the object's side.
  std::vector<std::array<double,2>> surfaceUVs; // Optional metric UVs, parallel to outline.

  // The segment this face is one side of, so the two sides of a wall can be
  // paired without measuring; UINT32_MAX for faces that have no other side.
  uint32_t segment = UINT32_MAX;

  // How far the far side is, for a face that has one: the wall's thickness.
  double thickness = 0;

  // What the segment carries, as a vertical extent above the segment's base:
  // a window's sill and head, a door's floor and head. Nought for a plain
  // segment. The mesh step draws the carried thing as a darker rectangle
  // between these; a later one can put joinery there.
  double low = 0, high = 0;

  // ...and its extent ALONG the segment, as fractions of the segment's
  // length. A window is a window's width, centred; a door piece is its own
  // width with a hair of jamb each side. Nought to nought for a plain
  // segment. The mesh cuts between these and nowhere else.
  double along0 = 0, along1 = 0;

  // Faces belonging to one structural item share a component. Walls use
  // `segment`; furniture uses this so its six faces become one Piece.
  uint32_t component = UINT32_MAX;
  TimberRole timberRole = TimberRole::Other;
  uint32_t support = UINT32_MAX; // First structure face carrying this component.
  std::vector<uint32_t> supports; // Every face around an aperture carrying it.
  rewrite::FurnitureKind furnitureKind = rewrite::FurnitureKind::Table;
  rewrite::Position facing{};
  bool siteSupport=false;
  uint32_t stairRisers=0;
  double stairSupportBase=0;
  bool stairWallSupported=false;
  ApertureShape aperture = ApertureShape::Rectangle;
  double archSpring = -1;
  bool detailedOpening = false;
  FacadeEdge leftEdge = FacadeEdge::Plain, rightEdge = FacadeEdge::Plain;
  std::vector<rewrite::Position> apertureBoundary;
  std::vector<std::vector<rewrite::Position>> wallPanels, openingReveals;
  bool passagePortal=false;
};

// An edge shared between faces - a corner, an eave, a ridge, a jamb. Carried
// from the start so a later mesh step can chamfer a corner or put a coping on
// an eave without either earlier process changing; the first mesh step reads
// faces only.
enum class StructureEdgeRole : uint8_t
{
  Corner,
  Eave,
  Ridge,
  Jamb,
  Sill,
  Head,
  Outline, // On the boundary of the structure: shared with nothing.
  Other
};

const char *name(StructureEdgeRole);

struct StructureEdge
{
  rewrite::Position a{}, b{};
  uint32_t faces[2] = {UINT32_MAX, UINT32_MAX};
  StructureEdgeRole role = StructureEdgeRole::Other;
};

// What a face belongs to. Object 0 is always the exterior; every room of every
// storey is an object of its own.
struct StructureObject
{
  std::string name;
  bool exterior = false;
  uint32_t storey = 0;
  uint32_t room = UINT32_MAX;
};

struct Structure
{
  std::vector<StructureObject> objects;
  std::vector<StructureFace> faces;
  std::vector<StructureEdge> edges;
  std::vector<FacadeInstance> instances;
  std::vector<rewrite::DwellingCoveredPassage> coveredPassages;
};

// The blueprint into a structure. Reads the plan and nothing else.
Structure buildStructure(const rewrite::DwellingPlan &);

// The edges of a structure, derived from its faces: which faces share each
// edge, and what the edge is. Filled into `Structure::edges` by
// `buildStructure`; exposed so a caller with faces of its own can ask again.
void deriveEdges(Structure &);

// The structure into faces the renderer draws. Reads the structure and
// nothing else - that is the whole contract, and it is what lets a better
// mesh step replace this one without the plan or the structure changing.
//
// One rule per kind, each the simplest that reads right: a wall segment is
// its two sides and a cap over the top; a windowed or doored segment is the
// same with a darker rectangle where the thing it carries is; a roof, a
// floor, a tread is its polygon. Nothing below ground is drawn, because the
// render has no earth.
void meshStructure(const Structure &, Solid &);

// The three vocabularies are CLOSED: every kind has a name, every material a
// thickness, and every kind has a mesh rule (see `meshStructure`). These are
// asked by the P24a gate.
bool vocabularyIsClosed(std::string *why = nullptr);

} // namespace procedural::world::hierarchy
