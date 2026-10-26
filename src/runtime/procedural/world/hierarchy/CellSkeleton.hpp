#pragma once

#include "procedural/world/hierarchy/CellStructure.hpp"

#include <cstdint>
#include <vector>

namespace procedural::world::hierarchy {

// The skeleton: the structure read as PIECES, and a graph of how they touch.
//
// The structure is faces with kinds, which is enough to draw. The mesh refactor
// wants more: a wall as one thing with two sides and four thin faces, a
// width, a height and a depth; and for every place two pieces meet, WHICH
// faces meet and WHERE on each piece, in that piece's own frame - so a corner
// can be mitred, a skirting run along a wall-floor edge, a coping put where a
// wall meets the sky, a hole cut in a floor where a ramp arrives, all without
// the mesh measuring geometry to find out what touches what.
//
// Built from the structure and nothing else. A piece's faces ARE structure
// faces (by index); the thin faces of a wall are derived from its two sides.
enum class PieceKind : uint8_t
{
  Wall, // Two-sided: every wall-ish segment, fences and rails included.
  Floor,
  Paving,
  Roof, // Roof, gable, soffit, cap.
  Deck,
  Ramp,
  Landing,
  Pier,
  Post,
  Timber,
  Furniture,
  Dormer,
  Count
};

const char *name(PieceKind);

enum class PieceSide : uint8_t
{
  Outside,
  Inside
};

// The thin faces of a wall piece: the ones that overlap something else.
enum class PieceFace : uint8_t
{
  Up,     // Meets the ceiling, the floor above, or the roof.
  Bottom, // Sits on the floor.
  Left,   // Meets the wall before it along the run (or a jamb).
  Right,  // ...and after it.
  Outside,
  Inside,
  Body // Any face of a piece that is not a wall.
};

const char *name(PieceFace);

struct Piece
{
  PieceKind kind = PieceKind::Wall;
  FaceKind faceKind = FaceKind::Wall; // What the structure called it: WindowedWall, Fence, Roof...
  Material material = Material::Stone;
  rewrite::FurnitureKind furnitureKind = rewrite::FurnitureKind::Table;
  rewrite::Position facing{};
  uint32_t segment = UINT32_MAX;     // Walls: the segment both sides share.
  uint32_t object = 0;               // The object the OUTSIDE face belongs to.
  uint32_t objectInside = 0;         // ...and the inside face, for a wall.
  bool party = false;

  // Structure face indices: a wall's two sides, or every face of another kind.
  uint32_t outside = UINT32_MAX;
  uint32_t inside = UINT32_MAX;
  std::vector<uint32_t> faces;

  // The piece's own frame: `origin` at the start of the outside face's base,
  // `u` along the wall, up is +y, `w` through it toward the inside. Other
  // kinds use the world axes from their first vertex.
  rewrite::Position origin{};
  double ux = 1, uz = 0;
  double wx = 0, wz = 1;

  double width = 0;  // Along.
  double height = 0; // Up.
  double depth = 0;  // Through: the thickness.

  // Local coordinates of a world point: (along, up, through).
  void local(const rewrite::Position &p, double &u, double &v, double &w) const;
  rewrite::Position world(double u, double v, double w) const;

  // A wall's faces by name, as world polygons. Up/Bottom/Left/Right are the
  // thin faces between the two sides; Outside/Inside the sides themselves.
  std::vector<rewrite::Position> face(PieceFace) const;

  // The wall-piece API. Kept as accessors even though the measurements are
  // public data too: later mesh builders should ask a piece what it is rather
  // than learn how its frame is stored. For a non-wall piece `getSide` is
  // empty; non-wall body polygons remain in the owned Structure.
  std::vector<rewrite::Position> getSide(PieceSide) const;
  std::vector<rewrite::Position> getFace(PieceFace) const;
  double getWidth() const { return width; }
  double getHeight() const { return height; }
  double getDepth() const { return depth; }
};

enum class ContactKind : uint8_t
{
  Corner,  // Two walls meeting at an upright edge, Left to Right.
  Jamb,    // A carried segment beside a plain one, along the same wall.
  Run,     // Two plain segments end to end along one wall.
  Stands,  // A wall's Bottom on a floor, a landing, a deck or paving.
  Capped,  // A wall's Up under a floor, a roof, a deck - or the sky.
  Arrives, // A ramp's head on a landing, a floor or a deck; its foot too.
  Meets    // Anything else that shares an edge.
};

const char *name(ContactKind);

// One place two pieces touch, from `a`'s point of view; every contact has its
// mirror from `b`'s. `b == UINT32_MAX` with `Capped` is the sky.
struct Contact
{
  uint32_t a = 0, b = UINT32_MAX;
  ContactKind kind = ContactKind::Meets;
  PieceFace faceOfA = PieceFace::Body;
  PieceFace faceOfB = PieceFace::Body;
  rewrite::Position at{};       // Where, in the world.
  double ua = 0, va = 0, wa = 0; // ...and in a's frame,
  double ub = 0, vb = 0, wb = 0; // ...and in b's.
};

// The other graph the structural phase owns: circulation between rooms. The
// blueprint says which thresholds and stairs were planned; the structural
// model turns them into stable nodes and edges so a later mesh/detail pass can
// place door assemblies, stairs, signs or lighting without reconstructing
// adjacency from coincident polygons.
enum class RoomConnectionKind : uint8_t
{
  Door,
  Passage,
  Stair,
  Exterior
};

const char *name(RoomConnectionKind);

struct RoomNode
{
  uint64_t id = 0;             // DwellingRoom::id.
  uint32_t blueprintRoom = 0;  // Index in DwellingPlan::rooms.
  uint32_t storey = 0;
  bool outdoor = false;
  std::vector<rewrite::Position> polygon;
};

struct RoomConnection
{
  uint32_t a = UINT32_MAX, b = UINT32_MAX; // UINT32_MAX is outside.
  RoomConnectionKind kind = RoomConnectionKind::Door;
  rewrite::Position at{};
  double width = 0;
};

struct Skeleton
{
  // One self-contained output of Structural Generation. Polygons and their
  // edge identities live here beside the pieces and graphs derived from them;
  // Mesh Generation needs no blueprint and no second companion argument.
  Structure structure;
  std::vector<Piece> pieces;
  std::vector<Contact> contacts;

  // Which piece a structure face belongs to (UINT32_MAX for none).
  std::vector<uint32_t> pieceOfFace;

  // Adjacency indexes into `contacts`, one list per piece.
  std::vector<std::vector<uint32_t>> contactsOfPiece;

  std::vector<RoomNode> rooms;
  std::vector<RoomConnection> roomConnections;
  std::vector<std::vector<uint32_t>> connectionsOfRoom;
};

// The complete middle phase: blueprint -> structural skeleton. This is the
// production entry point; `skeletonOf` remains useful to tests and callers
// that already own typed structural polygons.
Skeleton buildSkeleton(const rewrite::DwellingPlan &);
Skeleton skeletonOf(Structure);

// The skeleton into faces the renderer draws (P25d). One rule per piece
// kind, and every decision about what touches what - which wall ends are
// open and get an end face, what a wall is capped by, where the returns of
// a cut run through the thickness - comes from the piece's own frame and
// the contact graph. No intersection or distance is computed here; that is
// the whole contract, and `no_geometry_measured_in_mesh` holds it.
void meshSkeleton(const Skeleton &, Solid &);

} // namespace procedural::world::hierarchy
