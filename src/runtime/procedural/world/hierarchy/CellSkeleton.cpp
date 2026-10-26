#include "procedural/world/hierarchy/CellSkeleton.hpp"
#include "procedural/world/hierarchy/FacadeAtlas.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom3/Prism.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace procedural::world::hierarchy {

using rewrite::Position;

const char *name(const PieceKind kind)
{
  switch (kind)
  {
    case PieceKind::Wall: return "wall";
    case PieceKind::Floor: return "floor";
    case PieceKind::Paving: return "paving";
    case PieceKind::Roof: return "roof";
    case PieceKind::Deck: return "deck";
    case PieceKind::Ramp: return "ramp";
    case PieceKind::Landing: return "landing";
    case PieceKind::Pier: return "pier";
    case PieceKind::Post: return "post";
    case PieceKind::Timber: return "timber";
    case PieceKind::Furniture: return "furniture";
    case PieceKind::Dormer: return "dormer";
    case PieceKind::Count: break;
  }

  return "?";
}

const char *name(const PieceFace face)
{
  switch (face)
  {
    case PieceFace::Up: return "up";
    case PieceFace::Bottom: return "bottom";
    case PieceFace::Left: return "left";
    case PieceFace::Right: return "right";
    case PieceFace::Outside: return "outside";
    case PieceFace::Inside: return "inside";
    case PieceFace::Body: return "body";
  }

  return "?";
}

const char *name(const ContactKind kind)
{
  switch (kind)
  {
    case ContactKind::Corner: return "corner";
    case ContactKind::Jamb: return "jamb";
    case ContactKind::Run: return "run";
    case ContactKind::Stands: return "stands";
    case ContactKind::Capped: return "capped";
    case ContactKind::Arrives: return "arrives";
    case ContactKind::Meets: return "meets";
  }

  return "?";
}

const char *name(const RoomConnectionKind kind)
{
  switch (kind)
  {
    case RoomConnectionKind::Door: return "door";
    case RoomConnectionKind::Passage: return "passage";
    case RoomConnectionKind::Stair: return "stair";
    case RoomConnectionKind::Exterior: return "exterior";
  }

  return "?";
}

void Piece::local(const Position &p, double &u, double &v, double &w) const
{
  const double dx = p.x - origin.x, dz = p.z - origin.z;
  u = dx * ux + dz * uz;
  v = p.y - origin.y;
  w = dx * wx + dz * wz;
}

Position Piece::world(const double u, const double v, const double w) const
{
  return Position{origin.x + ux * u + wx * w, origin.y + v, origin.z + uz * u + wz * w};
}

std::vector<Position> Piece::face(const PieceFace which) const
{
  // The box in its own frame: u in [0, width], v in [0, height], w in [0, depth].
  switch (which)
  {
    case PieceFace::Outside:
      return {world(0, 0, 0), world(width, 0, 0), world(width, height, 0), world(0, height, 0)};
    case PieceFace::Inside:
      return {world(width, 0, depth), world(0, 0, depth), world(0, height, depth), world(width, height, depth)};
    case PieceFace::Up:
      return {world(0, height, 0), world(width, height, 0), world(width, height, depth), world(0, height, depth)};
    case PieceFace::Bottom:
      return {world(width, 0, 0), world(0, 0, 0), world(0, 0, depth), world(width, 0, depth)};
    case PieceFace::Left:
      return {world(0, 0, depth), world(0, 0, 0), world(0, height, 0), world(0, height, depth)};
    case PieceFace::Right:
      return {world(width, 0, 0), world(width, 0, depth), world(width, height, depth), world(width, height, 0)};
    case PieceFace::Body:
      break;
  }

  return {};
}

std::vector<Position> Piece::getSide(const PieceSide which) const
{
  if (kind != PieceKind::Wall)
  {
    return {};
  }

  return face(which == PieceSide::Outside ? PieceFace::Outside : PieceFace::Inside);
}

std::vector<Position> Piece::getFace(const PieceFace which) const
{
  return face(which);
}

namespace {

bool wallish(const FaceKind kind)
{
  return kind == FaceKind::Wall || kind == FaceKind::WindowedWall || kind == FaceKind::DoorWall ||
         kind == FaceKind::DoubleDoorWall || kind == FaceKind::ArchWall || kind == FaceKind::Parapet ||
         kind == FaceKind::Fence || kind == FaceKind::Rail;
}

bool carries(const FaceKind kind)
{
  return kind == FaceKind::WindowedWall || kind == FaceKind::DoorWall || kind == FaceKind::DoubleDoorWall ||
         kind == FaceKind::ArchWall;
}

PieceKind pieceKindOf(const FaceKind kind)
{
  switch (kind)
  {
    case FaceKind::Floor: return PieceKind::Floor;
    case FaceKind::Ceiling: return PieceKind::Deck;
    case FaceKind::Paving: return PieceKind::Paving;
    case FaceKind::Foundation: return PieceKind::Paving;
    case FaceKind::Roof:
    case FaceKind::Gable:
    case FaceKind::Soffit:
    case FaceKind::Cap: return PieceKind::Roof;
    case FaceKind::Deck: return PieceKind::Deck;
    case FaceKind::Ramp: return PieceKind::Ramp;
    case FaceKind::Landing: return PieceKind::Landing;
    case FaceKind::Pier: return PieceKind::Pier;
    case FaceKind::Post: return PieceKind::Post;
    case FaceKind::Timber: return PieceKind::Timber;
    case FaceKind::Furniture: return PieceKind::Furniture;
    case FaceKind::DormerOpening:
    case FaceKind::DormerWall:
    case FaceKind::DormerWindow:
    case FaceKind::DormerRoof:
    case FaceKind::DormerCeiling: return PieceKind::Dormer;
    default: return PieceKind::Wall;
  }
}

bool insideXZ(const std::vector<Position> &ring, const double x, const double z)
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

double topOf(const std::vector<Position> &ring)
{
  double top = -1e300;

  for (const Position &p : ring)
  {
    top = std::max(top, p.y);
  }

  return top;
}

double bottomOf(const std::vector<Position> &ring)
{
  double bottom = 1e300;

  for (const Position &p : ring)
  {
    bottom = std::min(bottom, p.y);
  }

  return bottom;
}

} // namespace

Skeleton skeletonOf(Structure structure)
{
  Skeleton out;
  out.pieceOfFace.assign(structure.faces.size(), UINT32_MAX);

  // ---- walls: one piece per segment, from its two sides -------------------------
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> sides;

  for (uint32_t f = 0; f < uint32_t(structure.faces.size()); ++f)
  {
    const StructureFace &face = structure.faces[f];

    if (!wallish(face.kind) || face.segment == UINT32_MAX)
    {
      continue;
    }

    auto &pair = sides[face.segment];

    // 1-based so 0 means empty. (A guard here once compared the 1-based slot
    // with the 0-based index, and face nought - the first wall's outside -
    // went into the second slot with the first left empty: segment nought was
    // one-sided, and the mesh drew its window uncut.)
    if (pair.first == 0)
    {
      pair.first = f + 1;
    }
    else
    {
      pair.second = f + 1;
    }
  }

  for (const auto &[segment, pair] : sides)
  {
    if (pair.first == 0 || pair.second == 0)
    {
      continue;
    }

    // The OUTSIDE is the side belonging to the exterior when one does, else
    // the first; the inside the other.
    uint32_t a = pair.first - 1, b = pair.second - 1;

    const bool aExterior = structure.faces[a].object < structure.objects.size() && structure.objects[structure.faces[a].object].exterior;
    const bool bExterior = structure.faces[b].object < structure.objects.size() && structure.objects[structure.faces[b].object].exterior;

    if (!aExterior && bExterior)
    {
      std::swap(a, b);
    }

    const StructureFace &outside = structure.faces[a];
    const StructureFace &inside = structure.faces[b];

    if (outside.outline.size() != 4 || inside.outline.size() != 4)
    {
      continue;
    }

    Piece piece;
    piece.kind = PieceKind::Wall;
    piece.faceKind = outside.kind;
    piece.material = outside.material;
    piece.segment = segment;
    piece.object = outside.object;
    piece.objectInside = inside.object;
    piece.party = outside.party && inside.party;
    piece.outside = a;
    piece.inside = b;
    piece.faces = {a, b};

    // The frame from the outside face: origin at its base start, u along it.
    const Position &o = outside.outline[0];
    const Position &e = outside.outline[1];
    const double len = std::hypot(e.x - o.x, e.z - o.z);
    piece.origin = o;
    piece.ux = len > 1e-9 ? (e.x - o.x) / len : 1.;
    piece.uz = len > 1e-9 ? (e.z - o.z) / len : 0.;

    // Through: toward the inside face. The inside face's base runs the other
    // way; its second corner is opposite the outside face's first.
    const Position &q = inside.outline[1];
    const double tx = q.x - o.x, tz = q.z - o.z;
    const double along = tx * piece.ux + tz * piece.uz;
    const double px = tx - piece.ux * along, pz = tz - piece.uz * along;
    const double through = std::hypot(px, pz);
    piece.wx = through > 1e-9 ? px / through : -piece.uz;
    piece.wz = through > 1e-9 ? pz / through : piece.ux;

    piece.width = len;
    piece.height = topOf(outside.outline) - o.y;
    piece.depth = through > 1e-9 ? through : outside.thickness;

    out.pieceOfFace[a] = uint32_t(out.pieces.size());
    out.pieceOfFace[b] = uint32_t(out.pieces.size());
    out.pieces.push_back(std::move(piece));
  }

  // ---- everything else: one piece per face ---------------------------------------
  // Furniture is the exception: its six faces are one box and therefore one
  // piece, keyed by the component identity assigned by Structural Generation.
  std::map<uint32_t, std::vector<uint32_t>> furnitureFaces;

  for (uint32_t f = 0; f < uint32_t(structure.faces.size()); ++f)
  {
    if (structure.faces[f].kind == FaceKind::Furniture && structure.faces[f].component != UINT32_MAX)
    {
      furnitureFaces[structure.faces[f].component].push_back(f);
    }
  }

  for (const auto &[component, faces] : furnitureFaces)
  {
    if (faces.empty()) continue;
    const StructureFace &first = structure.faces[faces.front()];
    Piece piece;
    piece.kind = PieceKind::Furniture;
    piece.faceKind = FaceKind::Furniture;
    piece.material = first.material;
    piece.furnitureKind = first.furnitureKind;
    piece.facing = first.facing;
    piece.object = first.object;
    piece.objectInside = first.object;
    piece.faces = faces;

    double minX = 1e300, minY = 1e300, minZ = 1e300;
    double maxX = -1e300, maxY = -1e300, maxZ = -1e300;

    for (const uint32_t f : faces)
    {
      out.pieceOfFace[f] = uint32_t(out.pieces.size());
      for (const Position &p : structure.faces[f].outline)
      {
        minX = std::min(minX, p.x); minY = std::min(minY, p.y); minZ = std::min(minZ, p.z);
        maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y); maxZ = std::max(maxZ, p.z);
      }
    }

    piece.origin = Position{minX, minY, minZ};
    piece.width = maxX - minX;
    piece.height = maxY - minY;
    piece.depth = maxZ - minZ;
    out.pieces.push_back(std::move(piece));
  }

  // A dormer is not an object standing on an otherwise intact roof. Fold its
  // typed opening, shell, ridge, ceiling and roof planes into the carrying
  // roof assembly, together with every main-roof face left around the cut.
  // The mesh can still route each subface by FaceKind, while the skeleton now
  // answers that the projection is part of the roof rather than merely linked
  // to it by a support contact.
  std::map<uint32_t, std::vector<uint32_t>> dormerFaces;
  for (uint32_t f = 0; f < uint32_t(structure.faces.size()); ++f)
  {
    const FaceKind kind = structure.faces[f].kind;
    if ((kind == FaceKind::DormerOpening || kind == FaceKind::DormerWall || kind == FaceKind::DormerWindow ||
         kind == FaceKind::DormerRoof || kind == FaceKind::DormerCeiling) &&
        structure.faces[f].component != UINT32_MAX)
    {
      dormerFaces[structure.faces[f].component].push_back(f);
    }
  }

  for (const auto &[component, faces] : dormerFaces)
  {
    if (faces.empty()) continue;
    const StructureFace &first = structure.faces[faces.front()];
    std::vector<uint32_t> assemblyFaces = first.supports;
    assemblyFaces.insert(assemblyFaces.end(), faces.begin(), faces.end());
    std::sort(assemblyFaces.begin(), assemblyFaces.end());
    assemblyFaces.erase(std::unique(assemblyFaces.begin(), assemblyFaces.end()), assemblyFaces.end());
    Piece piece;
    piece.kind = PieceKind::Roof;
    piece.faceKind = FaceKind::Roof;
    piece.material = Material::Tile;
    piece.object = first.object;
    piece.objectInside = first.object;
    piece.faces = assemblyFaces;
    double minX = 1e300, minY = 1e300, minZ = 1e300;
    double maxX = -1e300, maxY = -1e300, maxZ = -1e300;
    for (const uint32_t f : assemblyFaces)
    {
      out.pieceOfFace[f] = uint32_t(out.pieces.size());
      for (const Position &p : structure.faces[f].outline)
      {
        minX = std::min(minX, p.x); minY = std::min(minY, p.y); minZ = std::min(minZ, p.z);
        maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y); maxZ = std::max(maxZ, p.z);
      }
    }
    piece.origin = Position{minX, minY, minZ};
    piece.width = maxX - minX;
    piece.height = maxY - minY;
    piece.depth = maxZ - minZ;
    out.pieces.push_back(std::move(piece));
  }

  for (uint32_t f = 0; f < uint32_t(structure.faces.size()); ++f)
  {
    if (out.pieceOfFace[f] != UINT32_MAX)
    {
      continue;
    }

    const StructureFace &face = structure.faces[f];

    if (face.outline.size() < 3)
    {
      continue;
    }

    Piece piece;
    piece.kind = wallish(face.kind) ? PieceKind::Wall : pieceKindOf(face.kind);
    piece.faceKind = face.kind;
    piece.material = face.material;
    piece.object = face.object;
    piece.objectInside = face.object;
    piece.faces = {f};
    piece.origin = face.outline[0];
    piece.ux = 1;
    piece.uz = 0;
    piece.wx = 0;
    piece.wz = 1;

    double minU = 1e300, maxU = -1e300, minW = 1e300, maxW = -1e300;

    for (const Position &p : face.outline)
    {
      minU = std::min(minU, p.x - piece.origin.x);
      maxU = std::max(maxU, p.x - piece.origin.x);
      minW = std::min(minW, p.z - piece.origin.z);
      maxW = std::max(maxW, p.z - piece.origin.z);
    }

    piece.width = maxU - minU;
    piece.depth = maxW - minW;
    piece.height = topOf(face.outline) - bottomOf(face.outline);

    out.pieceOfFace[f] = uint32_t(out.pieces.size());
    out.pieces.push_back(std::move(piece));
  }

  // ---- contacts -------------------------------------------------------------------
  const auto add = [&](const uint32_t a, const uint32_t b, const ContactKind kind, const PieceFace fa, const PieceFace fb, const Position &at) {
    Contact c;
    c.a = a;
    c.b = b;
    c.kind = kind;
    c.faceOfA = fa;
    c.faceOfB = fb;
    c.at = at;
    out.pieces[a].local(at, c.ua, c.va, c.wa);

    if (b != UINT32_MAX)
    {
      out.pieces[b].local(at, c.ub, c.vb, c.wb);
    }

    out.contacts.push_back(c);

    if (b != UINT32_MAX)
    {
      Contact m = c;
      std::swap(m.a, m.b);
      std::swap(m.faceOfA, m.faceOfB);
      std::swap(m.ua, m.ub);
      std::swap(m.va, m.vb);
      std::swap(m.wa, m.wb);
      out.contacts.push_back(m);
    }
  };

  // Wall to wall, from the structure's own edges: a shared upright edge is a
  // corner or a jamb, and which of each wall's ends it is at says Left or
  // Right.
  for (const StructureEdge &edge : structure.edges)
  {
    if (edge.faces[1] == UINT32_MAX)
    {
      continue;
    }

    const uint32_t pa = out.pieceOfFace[edge.faces[0]];
    const uint32_t pb = out.pieceOfFace[edge.faces[1]];

    if (pa == UINT32_MAX || pb == UINT32_MAX || pa == pb)
    {
      continue;
    }

    const Piece &A = out.pieces[pa];
    const Piece &B = out.pieces[pb];

    if (A.kind != PieceKind::Wall || B.kind != PieceKind::Wall)
    {
      continue;
    }

    const bool upright = std::abs(edge.a.x - edge.b.x) < 1e-6 && std::abs(edge.a.z - edge.b.z) < 1e-6;

    if (!upright)
    {
      continue;
    }

    const Position at{edge.a.x, std::min(edge.a.y, edge.b.y), edge.a.z};
    double ua, va, wa, ub, vb, wb;
    A.local(at, ua, va, wa);
    B.local(at, ub, vb, wb);

    const PieceFace fa = ua < A.width * .5 ? PieceFace::Left : PieceFace::Right;
    const PieceFace fb = ub < B.width * .5 ? PieceFace::Left : PieceFace::Right;

    ContactKind kind = ContactKind::Meets;

    if (edge.role == StructureEdgeRole::Corner)
    {
      kind = ContactKind::Corner;
    }
    else if (edge.role == StructureEdgeRole::Jamb)
    {
      kind = ContactKind::Jamb;
    }
    else if (A.segment != UINT32_MAX && B.segment != UINT32_MAX && !carries(A.faceKind) && !carries(B.faceKind))
    {
      kind = ContactKind::Run;
    }

    add(pa, pb, kind, fa, fb, at);
  }

  // Walls to what they stand on and what caps them. Asked at the middle of the
  // wall's bottom and top: the level piece whose polygon holds that point at
  // that height. Nothing above is the sky, named as such.
  for (uint32_t p = 0; p < uint32_t(out.pieces.size()); ++p)
  {
    const Piece &wall = out.pieces[p];

    if (wall.kind != PieceKind::Wall)
    {
      continue;
    }

    const Position foot = wall.world(wall.width * .5, 0, wall.depth * .5);
    const Position head = wall.world(wall.width * .5, wall.height, wall.depth * .5);

    uint32_t under = UINT32_MAX;
    uint32_t over = UINT32_MAX;

    // A wall's centreline is a cell's edge, so the middle of its foot lies ON
    // the line between two floor squares - or, for an outside wall and a
    // fence, between a floor square and none. Asked a little to each side of
    // the centre as well, inside first.
    // Along the whole foot as well as across it: a pier a hand wide beside
    // a doorway has its middle on a cell corner, where nothing is inside
    // anything.
    std::vector<Position> feet;

    for (const double along : {.5, .2, .8})
    {
      for (const double across : {.08, -.08, 0.})
      {
        feet.push_back(wall.world(wall.width * along, 0, wall.depth * .5 + across));
      }
    }

    for (uint32_t q = 0; q < uint32_t(out.pieces.size()); ++q)
    {
      const Piece &other = out.pieces[q];

      if (q == p || other.faces.empty())
      {
        continue;
      }

      const std::vector<Position> &ring = structure.faces[other.faces.front()].outline;

      const bool level = other.kind == PieceKind::Floor || other.kind == PieceKind::Paving || other.kind == PieceKind::Deck ||
                         other.kind == PieceKind::Landing;

      if (level && under == UINT32_MAX && std::abs(topOf(ring) - foot.y) < .1)
      {
        for (const Position &probe : feet)
        {
          if (insideXZ(ring, probe.x, probe.z))
          {
            under = q;
            break;
          }
        }
      }

      const bool caps = level || other.kind == PieceKind::Roof;

      if (caps && over == UINT32_MAX && std::abs(bottomOf(ring) - head.y) < .1 &&
          (insideXZ(ring, head.x, head.z) || (other.kind == PieceKind::Roof && insideXZ(ring, head.x + wall.wx * .05, head.z + wall.wz * .05))))
      {
        over = q;
      }
    }

    // A wall on a wall: the storey above stands on the storey below.
    if (under == UINT32_MAX)
    {
      for (uint32_t q = 0; q < uint32_t(out.pieces.size()); ++q)
      {
        const Piece &other = out.pieces[q];

        if (q == p || other.kind != PieceKind::Wall)
        {
          continue;
        }

        double u, v, w;
        other.local(foot, u, v, w);

        if (std::abs(v - other.height) < .1 && u > -1e-3 && u < other.width + 1e-3 && w > -1e-3 && w < other.depth + 1e-3)
        {
          under = q;
          break;
        }
      }
    }

    if (under != UINT32_MAX)
    {
      add(p, under, ContactKind::Stands, PieceFace::Bottom, PieceFace::Body, foot);
    }

    if (over != UINT32_MAX)
    {
      add(p, over, ContactKind::Capped, PieceFace::Up, PieceFace::Body, head);
    }
    else
    {
      // Capped by a wall above, or by the sky.
      uint32_t above = UINT32_MAX;

      for (uint32_t q = 0; q < uint32_t(out.pieces.size()); ++q)
      {
        const Piece &other = out.pieces[q];

        if (q == p || other.kind != PieceKind::Wall)
        {
          continue;
        }

        double u, v, w;
        other.local(head, u, v, w);

        if (std::abs(v) < .1 && u > -1e-3 && u < other.width + 1e-3 && w > -1e-3 && w < other.depth + 1e-3)
        {
          above = q;
          break;
        }
      }

      add(p, above, ContactKind::Capped, PieceFace::Up, above == UINT32_MAX ? PieceFace::Body : PieceFace::Bottom, head);
    }
  }

  // Ramps to what they leave from and arrive on.
  for (uint32_t p = 0; p < uint32_t(out.pieces.size()); ++p)
  {
    const Piece &ramp = out.pieces[p];

    if (ramp.kind != PieceKind::Ramp)
    {
      continue;
    }

    const std::vector<Position> &ring = structure.faces[ramp.faces.front()].outline;

    if (ring.size() != 4)
    {
      continue;
    }

    const Position foot{(ring[0].x + ring[1].x) * .5, ring[0].y, (ring[0].z + ring[1].z) * .5};
    const Position head{(ring[2].x + ring[3].x) * .5, ring[2].y, (ring[2].z + ring[3].z) * .5};
    const double run = std::max(1e-9, std::hypot(head.x - foot.x, head.z - foot.z));
    const double ux = (head.x - foot.x) / run, uz = (head.z - foot.z) / run;

    for (const bool atHead : {false, true})
    {
      const Position probe = atHead ? Position{head.x + ux * .3, head.y, head.z + uz * .3}
                                    : Position{foot.x - ux * .3, foot.y, foot.z - uz * .3};

      for (uint32_t q = 0; q < uint32_t(out.pieces.size()); ++q)
      {
        const Piece &other = out.pieces[q];

        if (q == p || other.faces.empty())
        {
          continue;
        }

        const bool level = other.kind == PieceKind::Floor || other.kind == PieceKind::Deck || other.kind == PieceKind::Landing;
        const std::vector<Position> &oring = structure.faces[other.faces.front()].outline;

        if (level && std::abs(topOf(oring) - probe.y) < .1 && insideXZ(oring, probe.x, probe.z))
        {
          add(p, q, ContactKind::Arrives, PieceFace::Body, PieceFace::Body, atHead ? head : foot);
          break;
        }
      }
    }
  }

  out.contactsOfPiece.resize(out.pieces.size());

  for (uint32_t c = 0; c < uint32_t(out.contacts.size()); ++c)
  {
    if (out.contacts[c].a < out.contactsOfPiece.size())
    {
      out.contactsOfPiece[out.contacts[c].a].push_back(c);
    }
  }

  out.structure = std::move(structure);

  return out;
}

Skeleton buildSkeleton(const rewrite::DwellingPlan &plan)
{
  Skeleton out = skeletonOf(buildStructure(plan));
  out.rooms.reserve(plan.rooms.size());
  out.connectionsOfRoom.resize(plan.rooms.size());

  std::map<uint64_t, uint32_t> roomById;

  for (uint32_t r = 0; r < uint32_t(plan.rooms.size()); ++r)
  {
    const rewrite::DwellingRoom &room = plan.rooms[r];
    RoomNode node;
    node.id = room.id;
    node.blueprintRoom = r;
    node.storey = room.floor;
    node.outdoor = room.kind == rewrite::RoomKind::Yard || room.kind == rewrite::RoomKind::Courtyard ||
                   room.kind == rewrite::RoomKind::Pen;
    node.polygon = room.boundary;
    roomById[room.id] = r;
    out.rooms.push_back(std::move(node));
  }

  const auto addRoomConnection = [&](const uint32_t a, const uint32_t b, const RoomConnectionKind kind,
                                     const Position &at, const double width) {
    // A pair of leaves or flights may describe the same passage. The room
    // graph is topological, so keep one edge for one kind and room pair.
    const uint32_t lo = std::min(a, b), hi = std::max(a, b);

    for (const RoomConnection &old : out.roomConnections)
    {
      if (old.kind == kind && std::min(old.a, old.b) == lo && std::max(old.a, old.b) == hi)
      {
        return;
      }
    }

    RoomConnection edge;
    edge.a = a;
    edge.b = b;
    edge.kind = kind;
    edge.at = at;
    edge.width = width;
    const uint32_t index = uint32_t(out.roomConnections.size());
    out.roomConnections.push_back(edge);

    if (a < out.connectionsOfRoom.size()) out.connectionsOfRoom[a].push_back(index);
    if (b < out.connectionsOfRoom.size()) out.connectionsOfRoom[b].push_back(index);
  };

  for (const rewrite::DwellingDoor &door : plan.doors)
  {
    const auto aIt = roomById.find(door.room);
    const auto bIt = roomById.find(door.fromRoom);
    const uint32_t a = aIt == roomById.end() ? UINT32_MAX : aIt->second;
    const uint32_t b = door.exterior || bIt == roomById.end() ? UINT32_MAX : bIt->second;

    if (a == UINT32_MAX && b == UINT32_MAX)
    {
      continue;
    }

    const RoomConnectionKind kind = door.exterior ? RoomConnectionKind::Exterior
                                                   : (door.privateAccess ? RoomConnectionKind::Door : RoomConnectionKind::Passage);
    addRoomConnection(a, b, kind, door.center, door.width);
  }

  const auto roomAt = [&](const uint32_t floor, const Position &p) {
    for (uint32_t r = 0; r < uint32_t(out.rooms.size()); ++r)
    {
      if (out.rooms[r].storey == floor && out.rooms[r].polygon.size() >= 3 && insideXZ(out.rooms[r].polygon, p.x, p.z))
      {
        return r;
      }
    }

    return UINT32_MAX;
  };

  const auto centre = [](const std::vector<Position> &ring, const double fallbackY) {
    Position p{0, fallbackY, 0};

    if (ring.empty()) return p;

    p.y = 0;
    for (const Position &q : ring) { p.x += q.x; p.y += q.y; p.z += q.z; }
    const double n = double(ring.size());
    p.x /= n; p.y /= n; p.z /= n;
    return p;
  };

  for (const rewrite::DwellingStair &stair : plan.stairs)
  {
    const Position low = centre(stair.lowerLanding.empty() ? stair.opening : stair.lowerLanding, 0);
    const Position high = centre(stair.upperLanding.empty() ? stair.opening : stair.upperLanding, low.y + stair.rise * stair.risers);
    const uint32_t a = roomAt(stair.lowerFloor, low);
    const uint32_t b = roomAt(stair.lowerFloor + 1, high);

    if (a != UINT32_MAX || b != UINT32_MAX)
    {
      addRoomConnection(a, b, RoomConnectionKind::Stair,
                        Position{(low.x + high.x) * .5, (low.y + high.y) * .5, (low.z + high.z) * .5}, stair.width);
    }
  }

  return out;
}


// ---- the mesh --------------------------------------------------------------------

namespace {

bool carries(const Piece &piece)
{
  return piece.faceKind == FaceKind::WindowedWall || piece.faceKind == FaceKind::DoorWall ||
         piece.faceKind == FaceKind::DoubleDoorWall || piece.faceKind == FaceKind::ArchWall;
}

} // namespace

namespace {
Solid::SurfaceMaterial surfaceMaterial(Material material)
{
  using M=Solid::SurfaceMaterial;
  switch(material) {
    case Material::Stone:return M::Stone;
    case Material::Tile:return M::RoofTile;
    case Material::Timber:return M::Wood;
    case Material::Boards:return M::Boarding;
    case Material::Wattle:return M::HalfTimber;
    case Material::Earth:return M::Paving;
    default:return M::Unassigned;
  }
}

// A UV-only pass: it never moves, splits or repairs geometry. Each surface
// gets an orthonormal planar chart, including returns and thin arch facets.
// World-phase projections keep adjacent coplanar wall panels continuous.
void mapSolidSurfaces(Solid &solid)
{
  using M=Solid::SurfaceMaterial;
  uint32_t island=0;
  const auto group=[&](std::vector<Solid::Face> &faces,M fallback) {
    for(auto &face:faces) {
      ++island;
      if(face.size()<3)continue;
      double nx=0,ny=0,nz=0;
      for(size_t i=0;i<face.size();++i) {
        const auto &a=face[i],&b=face[(i+1)%face.size()];
        nx+=(a.y-b.y)*(a.z+b.z);ny+=(a.z-b.z)*(a.x+b.x);nz+=(a.x-b.x)*(a.y+b.y);
      }
      const double length=std::sqrt(nx*nx+ny*ny+nz*nz);
      if(length<1e-12)continue;
      nx/=length;ny/=length;nz/=length;
      const double horizontal=std::hypot(nx,nz);
      double ux=1,uy=0,uz=0,vx=0,vy=0,vz=1;
      if(horizontal>1e-8) {
        ux=-nz/horizontal;uz=nx/horizontal;
        if(ux< -1e-8 || (std::abs(ux)<1e-8 && uz<0)){ux=-ux;uz=-uz;}
        vx=ny*uz;vy=nz*ux-nx*uz;vz=-ny*ux;
        if(vy<0){vx=-vx;vy=-vy;vz=-vz;}
      }
      const auto material=face.front().material==M::Unassigned?fallback:face.front().material;
      if(material==M::Wood && face.size()>=4) {
        // Beam/plank grain follows the member's long edge, not world up.
        double longest=0;
        for(size_t i=0;i<face.size();++i) {
          const auto &a=face[i],&b=face[(i+1)%face.size()];
          const double x=b.x-a.x,y=b.y-a.y,z=b.z-a.z,l=x*x+y*y+z*z;
          if(l>longest){longest=l;vx=x;vy=y;vz=z;}
        }
        const double l=std::sqrt(longest);vx/=l;vy/=l;vz/=l;
        if(vy< -1e-8 || (std::abs(vy)<1e-8 && (vx< -1e-8 || (std::abs(vx)<1e-8 && vz<0)))){vx=-vx;vy=-vy;vz=-vz;}
        ux=vy*nz-vz*ny;uy=vz*nx-vx*nz;uz=vx*ny-vy*nx;
      }
      for(auto &p:face) {
        if(!p.authoredUV){p.u=p.x*ux+p.y*uy+p.z*uz;p.v=p.x*vx+p.y*vy+p.z*vz;}
        p.uvIsland=island;
        if(p.material==M::Unassigned)p.material=fallback;
      }
    }
  };
  group(solid.walls,M::Stone);group(solid.innerWalls,M::Stone);group(solid.gables,M::Stone);
  group(solid.slopes,M::RoofTile);group(solid.soffits,M::Wood);group(solid.pillars,M::Stone);
  group(solid.joinery,M::Glass);group(solid.reveals,M::Stone);group(solid.innerReveals,M::Stone);
  group(solid.decking,M::Wood);group(solid.stairs,M::Stone);group(solid.timbers,M::Wood);group(solid.furniture,M::Wood);
  group(solid.dormerWalls,M::Wood);group(solid.dormerWindows,M::Glass);group(solid.dormerRoofs,M::RoofTile);
  group(solid.dormerCeilings,M::Wood);group(solid.yard,M::Paving);group(solid.foundations,M::DarkStone);
}
}

void meshSkeleton(const Skeleton &skeleton, Solid &out)
{
  const Structure &structure = skeleton.structure;
  const auto vertex = [](const Position &p) { return Solid::Vertex{p.x, p.y, p.z}; };
  auto activeMaterial=Solid::SurfaceMaterial::Unassigned;

  const auto quad = [&](const std::vector<Position> &outline, std::vector<Solid::Face> &into) {
    Solid::Face face;

    for (const Position &p : outline)
    {
      face.push_back(vertex(p));
      face.back().material=activeMaterial;
    }

    into.push_back(std::move(face));
  };

  // Finished floors own their horizontal surface. Clip only the covered
  // part of a coplanar wall cap, retaining exposed shoulders at setbacks.
  std::map<int64_t,std::vector<geom2::Polygon>> finishes;
  const auto polygonXZ=[](const std::vector<Position> &ring) {
    geom2::Polygon p;
    for(auto q:ring)p.outer.push_back({geom2::toUnit(q.x),geom2::toUnit(q.z)});
    if(!geom2::counterClockwise(p.outer))geom2::reverse(p.outer);
    return p;
  };
  for(const auto &f:structure.faces)if((f.kind==FaceKind::Floor || f.kind==FaceKind::Paving || f.kind==FaceKind::Landing) && f.outline.size()>=3) {
    const double y=f.outline.front().y;
    if(std::all_of(f.outline.begin(),f.outline.end(),[&](auto p){return std::abs(p.y-y)<1e-7;}))
      finishes[std::llround(y*1000000.)].push_back(polygonXZ(f.outline));
  }
  const auto exposedCap=[&](const std::vector<Position> &ring,std::vector<Solid::Face> &into) {
    const auto at=finishes.find(std::llround(ring.front().y*1000000.));
    if(at==finishes.end()){quad(ring,into);return;}
    auto cap=polygonXZ(ring);
    std::vector<geom2::Operand> cuts;
    auto bounds=[](const geom2::Polygon &p){std::array<double,4> b{1e300,1e300,-1e300,-1e300};for(auto q:p.outer){b[0]=std::min(b[0],double(q.x));b[1]=std::min(b[1],double(q.y));b[2]=std::max(b[2],double(q.x));b[3]=std::max(b[3],double(q.y));}return b;};
    const auto cb=bounds(cap);
    for(const auto &floor:at->second){const auto b=bounds(floor);if(b[0]<cb[2] && b[2]>cb[0] && b[1]<cb[3] && b[3]>cb[1])cuts.push_back(geom2::toOperand(floor));}
    if(cuts.empty()){quad(ring,into);return;}
    std::vector<geom2::Polygon> exposed;
    if(!geom2::differenceOf(geom2::toOperand(cap),cuts,exposed,nullptr)){quad(ring,into);return;}
    for(const auto &p:exposed){std::vector<std::array<uint32_t,3>> triangles;std::vector<geom2::Point> vertices;
      if(!geom3::triangulate(p,triangles,vertices,nullptr))continue;
      for(auto t:triangles){std::vector<Position> face;for(auto i:t)face.push_back({geom2::toMetres(vertices[i].x),ring.front().y,geom2::toMetres(vertices[i].y)});std::reverse(face.begin(),face.end());quad(face,into);}
    }
  };

  const auto exteriorObject = [&](const uint32_t object) {
    return object < structure.objects.size() && structure.objects[object].exterior;
  };

  // What the graph says about each piece: which thin faces are touched, and
  // the wall standing on top of it if any.
  std::vector<std::array<uint8_t, 7>> touched(skeleton.pieces.size(), std::array<uint8_t, 7>{});
  std::vector<uint32_t> wallAbove(skeleton.pieces.size(), UINT32_MAX);

  for (const Contact &c : skeleton.contacts)
  {
    if (c.a >= skeleton.pieces.size())
    {
      continue;
    }

    touched[c.a][size_t(c.faceOfA)] = 1;

    if (c.kind == ContactKind::Capped && c.b != UINT32_MAX && c.faceOfB == PieceFace::Bottom)
    {
      wallAbove[c.a] = c.b;
    }
  }

  // One side of a wall, with the carried thing cut out of it between the
  // structure's own `along0/along1` and `low/high`.
  const auto side = [&](const StructureFace &face, std::vector<Solid::Face> &into) {
    activeMaterial=surfaceMaterial(face.material);
    if(face.detailedOpening) {
      for(const auto &polygon:face.wallPanels)quad(polygon,into);
      return;
    }
    const bool cut = (face.kind == FaceKind::WindowedWall || face.kind == FaceKind::DoorWall ||
                      face.kind == FaceKind::DoubleDoorWall || face.kind == FaceKind::ArchWall) &&
                     face.outline.size() == 4 && face.high > face.low + 1e-6 && face.along1 > face.along0 + 1e-6;

    if (!cut)
    {
      quad(face.outline, into);

      return;
    }

    const Position &a = face.outline[0];
    const Position &b = face.outline[1];
    const Position &d = face.outline[3];
    const double base = a.y, top = d.y;
    const double low = base + face.low, high = std::min(top, base + face.high);
    const double t0 = std::clamp(face.along0, 0., 1.), t1 = std::clamp(face.along1, 0., 1.);

    const auto at = [&](const double t, const double y) {
      return Position{a.x + (b.x - a.x) * t, y, a.z + (b.z - a.z) * t};
    };

    const auto panel = [&](const double u0, const double u1, const double y0, const double y1) {
      if (u1 - u0 < 1e-6 || y1 - y0 < 1e-6)
      {
        return;
      }

      quad({at(u0, y0), at(u1, y0), at(u1, y1), at(u0, y1)}, into);
    };

    if(face.detailedOpening) {
      for(const auto &polygon:face.wallPanels)quad(polygon,into);
      return;
    }

    panel(0., t0, base, top);  // the pier before the hole
    panel(t1, 1., base, top);  // ...and after it
    panel(t0, t1, base, low);  // under it (nothing for a door)
    panel(t0, t1, high, top);  // over it
  };

  for (uint32_t index = 0; index < uint32_t(skeleton.pieces.size()); ++index)
  {
    const Piece &piece = skeleton.pieces[index];

    if (piece.kind == PieceKind::Wall)
    {
      // A one-sided wallish face (a parapet with no inside, a side whose
      // partner is buried) is drawn as it is, if its top is above ground.
      if (piece.segment == UINT32_MAX || piece.outside >= structure.faces.size() || piece.inside >= structure.faces.size())
      {
        for (const uint32_t f : piece.faces)
        {
          if (f >= structure.faces.size())
          {
            continue;
          }

          double highest = -1e300;

          for (const Position &p : structure.faces[f].outline)
          {
            highest = std::max(highest, p.y);
          }

          if (highest > 1e-6)
          {
            side(structure.faces[f], exteriorObject(structure.faces[f].object) ? out.walls : out.innerWalls);
          }
        }

        continue;
      }

      // Nothing below ground: a cellar wall whose top is at nought is buried.
      if (piece.origin.y + piece.height <= 1e-6)
      {
        continue;
      }

      const StructureFace &outside = structure.faces[piece.outside];
      const StructureFace &inside = structure.faces[piece.inside];
      side(outside, exteriorObject(outside.object) ? out.walls : out.innerWalls);
      side(inside, exteriorObject(inside.object) ? out.walls : out.innerWalls);

      const bool exterior = exteriorObject(outside.object) || exteriorObject(inside.object);
      std::vector<Solid::Face> &returns = exterior ? out.reveals : out.innerReveals;

      if (outside.outline.size() != 4 || inside.outline.size() != 4)
      {
        continue;
      }

      const std::vector<Position> &r = outside.outline; // a, b along the base, c, d along the top
      std::vector<Position> l = inside.outline;
      // Cap/return stitching needs the inside edge in the opposite direction.
      // Short mitred segments can arrive with both skins oriented alike;
      // assuming otherwise connects opposite corners into a bow-tie cap.
      if ((r[1].x-r[0].x)*(l[1].x-l[0].x)+(r[1].z-r[0].z)*(l[1].z-l[0].z)>0)
        l={inside.outline[1],inside.outline[0],inside.outline[3],inside.outline[2]};

      // The cap: between the two sides' top edges - unless the graph says a
      // wall at least as thick stands on it, when nobody can see it.
      const bool hidden = wallAbove[index] != UINT32_MAX && wallAbove[index] < skeleton.pieces.size() &&
                          skeleton.pieces[wallAbove[index]].depth >= piece.depth - 1e-6;

      if (!hidden)
      {
        exposedCap({r[3], r[2], l[3], l[2]}, returns);
      }

      // The ends: where the graph names nothing on Left or Right, the wall
      // ends in the air (or at a T-junction), and it is closed with the face
      // between the two sides' end edges. A mitred corner, a jamb and a run
      // are all touched and need nothing.
      // ...but not where the end is buried in the SIDE of another wall.
      //
      // `touched` is end-to-end adjacency, so a partition running into the
      // face of the wall it dies against counts as untouched and gets a cap.
      // That cap is a quad the partition's own thickness wide and a storey
      // tall, lying flush in the other wall's skin - redundant, because the
      // wall it abuts already closes that end, and a z-fight, which is the
      // narrow dark rectangle standing on a wall.
      const auto buriedEnd = [&](const bool atStart) {
        const Piece &self = skeleton.pieces[index];
        const double along = atStart ? -.03 : self.width + .03;
        const double px = self.origin.x + self.ux * along + self.wx * self.depth * .5;
        const double pz = self.origin.z + self.uz * along + self.wz * self.depth * .5;
        const double py = self.origin.y + std::min(self.height, 1.) * .5;

        for (size_t other = 0; other < skeleton.pieces.size(); ++other)
        {
          if (other == index || skeleton.pieces[other].kind != PieceKind::Wall)
          {
            continue;
          }

          const Piece &wall = skeleton.pieces[other];

          if (py < wall.origin.y - 1e-6 || py > wall.origin.y + wall.height + 1e-6)
          {
            continue;
          }

          const double dx = px - wall.origin.x, dz = pz - wall.origin.z;
          const double u = dx * wall.ux + dz * wall.uz;
          const double w = dx * wall.wx + dz * wall.wz;

          if (u >= -1e-6 && u <= wall.width + 1e-6 && w >= -1e-6 && w <= wall.depth + 1e-6)
          {
            return true;
          }
        }

        return false;
      };

      if (touched[index][size_t(PieceFace::Left)] == 0 && !buriedEnd(true))
      {
        quad({l[1], r[0], r[3], l[2]}, returns);
      }

      if (touched[index][size_t(PieceFace::Right)] == 0 && !buriedEnd(false))
      {
        quad({r[1], l[0], l[3], r[2]}, returns);
      }

      // The returns of a cut, from the piece's own frame: the hole is at
      // `along0..along1` of the outside face - which IS the frame's u - between
      // `low` and `high` up it, and runs through the whole depth.
      if (carries(piece) && outside.high > outside.low + 1e-6 && outside.along1 > outside.along0 + 1e-6)
      {
        const double u0 = std::clamp(outside.along0, 0., 1.) * piece.width;
        const double u1 = std::clamp(outside.along1, 0., 1.) * piece.width;
        const double v0 = outside.low, v1 = std::min(piece.height, outside.high);
        const double w = piece.depth;

        if(outside.detailedOpening) {
          for(const auto &polygon:outside.openingReveals)quad(polygon,returns);
          continue;
        }

        quad({piece.world(u0, v0, 0), piece.world(u0, v0, w), piece.world(u0, v1, w), piece.world(u0, v1, 0)}, returns); // jamb
        quad({piece.world(u1, v0, w), piece.world(u1, v0, 0), piece.world(u1, v1, 0), piece.world(u1, v1, w)}, returns); // jamb
        quad({piece.world(u0, v1, 0), piece.world(u1, v1, 0), piece.world(u1, v1, w), piece.world(u0, v1, w)}, returns); // head

        if (v0 > 1e-6)
        {
          quad({piece.world(u1, v0, 0), piece.world(u0, v0, 0), piece.world(u0, v0, w), piece.world(u1, v0, w)}, returns); // sill
        }
      }

      continue;
    }

    // Everything else: one rule per kind, from the piece's faces.
    for (const uint32_t f : piece.faces)
    {
      if (f >= structure.faces.size())
      {
        continue;
      }

      const StructureFace &face = structure.faces[f];
      activeMaterial=surfaceMaterial(face.material);
      if(face.engagedArch || (face.masonryVault && face.kind==FaceKind::Pier))activeMaterial=Solid::SurfaceMaterial::CarvedStone;
      if(face.masonryVault && face.kind==FaceKind::Ceiling)activeMaterial=Solid::SurfaceMaterial::VaultMasonry;
      if(face.siteSupport) {activeMaterial=face.pavedSupportTop ? Solid::SurfaceMaterial::Paving : Solid::SurfaceMaterial::DarkStone;quad(face.outline,out.foundations);continue;}
      double highest = -1e300;

      for (const Position &p : face.outline)
      {
        highest = std::max(highest, p.y);
      }

      if ((highest < -1e-6 && face.kind != FaceKind::Ceiling) ||
          (highest <= 1e-6 && face.kind == FaceKind::Timber))
      {
        continue;
      }

      switch (face.kind)
      {
        case FaceKind::Roof:
        case FaceKind::Deck:
        case FaceKind::Cap:
          quad(face.outline, out.slopes);
          break;
        case FaceKind::Gable:
          quad(face.outline, out.gables);
          break;
        case FaceKind::Soffit:
        case FaceKind::Ceiling:
          quad(face.outline, out.soffits);
          if(face.surfaceUVs.size()==face.outline.size())for(size_t i=0;i<face.surfaceUVs.size();++i) {
            auto &vertex=out.soffits.back()[i];vertex.u=face.surfaceUVs[i][0];vertex.v=face.surfaceUVs[i][1];vertex.authoredUV=true;
          }
          break;
        case FaceKind::Pier:
          quad(face.outline, out.walls);
          break;
        case FaceKind::Post:
          quad(face.outline, out.pillars);
          break;
        case FaceKind::Floor:
          quad(face.outline, out.decking);
          break;
        case FaceKind::Paving:
          quad(face.outline, out.yard);
          break;
        case FaceKind::Ramp:
        {
          if(face.outline.size()!=4){quad(face.outline,out.stairs);break;}
          const auto a=face.outline[0],b=face.outline[1],c=face.outline[2],d=face.outline[3];
          const uint32_t risers=std::max(2u,face.stairRisers);
          const double rise=d.y-a.y,bottom=face.stairSupportBase;
          const auto at=[&](Position p,Position q,double t,double y){return Position{p.x+(q.x-p.x)*t,y,p.z+(q.z-p.z)*t};};
          if (face.stairWallSupported) {
            // Closed, shallow tread blocks; the shaft wall supports the outer
            // edge, so no vertical mass obstructs flights on the turns below.
            for (uint32_t k=0;k<risers;++k) {
              const double t0=double(k)/risers,t1=double(k+1)/risers;
              const double y=a.y+rise*t1,low=y-.18;
              auto l0=at(a,d,t0,y),r0=at(b,c,t0,y),l1=at(a,d,t1,y),r1=at(b,c,t1,y);
              auto bl0=at(a,d,t0,low),br0=at(b,c,t0,low),bl1=at(a,d,t1,low),br1=at(b,c,t1,low);
              quad({l0,l1,r1,r0},out.stairs);
              quad({bl0,br0,br1,bl1},out.stairs);
              quad({bl0,l0,r0,br0},out.stairs);
              quad({bl1,br1,r1,l1},out.stairs);
              quad({bl0,bl1,l1,l0},out.stairs);
              quad({br0,r0,r1,br1},out.stairs);
            }
            break;
          }
          for(uint32_t k=0;k<risers-1;++k){
            const double t0=double(k)/(risers-1),t1=double(k+1)/(risers-1);
            const double y=a.y+rise*double(k+1)/risers,previous=a.y+rise*double(k)/risers;
            const auto l0=at(a,d,t0,y),r0=at(b,c,t0,y),l1=at(a,d,t1,y),r1=at(b,c,t1,y);
            quad({l0,l1,r1,r0},out.stairs);
            quad({at(a,d,t0,previous),l0,r0,at(b,c,t0,previous)},out.stairs);
            quad({at(a,d,t0,bottom),at(a,d,t1,bottom),l1,l0},out.stairs);
            quad({at(b,c,t0,bottom),r0,r1,at(b,c,t1,bottom)},out.stairs);
          }
          quad({at(a,d,1,a.y+rise*double(risers-1)/risers),d,c,at(b,c,1,a.y+rise*double(risers-1)/risers)},out.stairs);
          quad({at(a,d,1,bottom),at(b,c,1,bottom),c,d},out.stairs);
          quad({at(a,d,0,bottom),at(b,c,0,bottom),at(b,c,1,bottom),at(a,d,1,bottom)},out.stairs);
          if(a.y>bottom+1e-6)quad({at(a,d,0,bottom),a,b,at(b,c,0,bottom)},out.stairs);

          break;
        }
        case FaceKind::Landing:
          quad(face.outline, out.stairs);
          break;
        case FaceKind::Timber:
          // Ridge ornaments belong to the roof for cutaway/visibility control.
          quad(face.outline, face.timberRole==TimberRole::Ridge ? out.slopes : out.timbers);
          break;
        case FaceKind::Furniture:
          quad(face.outline, out.furniture);
          break;
        case FaceKind::DormerOpening:
          // An aperture in the main roof, deliberately not a surface.
          break;
        case FaceKind::DormerWall:
          quad(face.outline, out.dormerWalls);
          break;
        case FaceKind::DormerWindow:
          quad(face.outline, out.dormerWindows);
          break;
        case FaceKind::DormerRoof:
          quad(face.outline, out.dormerRoofs);
          break;
        case FaceKind::DormerCeiling:
          quad(face.outline, out.dormerCeilings);
          break;
        default:
          break;
      }
    }
  }
  meshFacadeInstances(structure,out);
  mapSolidSurfaces(out);
}

} // namespace procedural::world::hierarchy
