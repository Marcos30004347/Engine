#include "procedural/world/hierarchy/DwellingVoxelAssembly.hpp"

#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural::world::hierarchy {
namespace {

bool fail(std::string *error, const char *message)
{
  if (error) *error = message;
  return false;
}

void mix(uint64_t &hash, uint64_t value)
{
  for (unsigned byte = 0; byte < 8; ++byte)
  {
    hash ^= uint8_t(value >> (byte * 8));
    hash *= 1099511628211ull;
  }
}

int64_t micron(double metres)
{
  return std::llround(metres * 1000000.0);
}

struct LatticeFrame
{
  rewrite::Position origin{}, u{}, v{};
  double pitch = 0;

  std::array<double, 3> local(const rewrite::Position &point) const
  {
    const double dx = point.x - origin.x, dz = point.z - origin.z;
    const double ux = u.x, uz = u.z, vx = v.x, vz = v.z;
    const double determinant = ux * vz - uz * vx;
    return {(dx * vz - dz * vx) / determinant,
            point.y,
            (ux * dz - uz * dx) / determinant};
  }

  bool place(const rewrite::Position &point, DwellingVoxelPart &part) const
  {
    const auto p = local(point);
    for (unsigned axis = 0; axis < 3; ++axis)
    {
      const double scaled = p[axis] / pitch;
      if (!std::isfinite(scaled) || scaled < double(std::numeric_limits<int32_t>::min()) ||
          scaled > double(std::numeric_limits<int32_t>::max())) return false;
      part.gridPosition[axis] = static_cast<int32_t>(std::llround(scaled));
      part.residualMetres[axis] = p[axis] - double(part.gridPosition[axis]) * pitch;
    }
    return true;
  }

  bool place(const rewrite::Position &point, DwellingVoxelConnector &connector) const
  {
    DwellingVoxelPart p;
    if (!place(point, p)) return false;
    connector.gridPosition = p.gridPosition;
    connector.residualMetres = p.residualMetres;
    return true;
  }

  void orientation(double x, double z, DwellingVoxelPart &part) const
  {
    const double length = std::hypot(x, z);
    if (length < 1e-12) { part.requiresUniqueOrientation = true; return; }
    part.localAngleRadians = std::atan2(x * v.x + z * v.z, x * u.x + z * u.z);
    const double vectors[4][2] = {{u.x,u.z},{v.x,v.z},{-u.x,-u.z},{-v.x,-v.z}};
    double best = -2;
    for (uint8_t turn = 0; turn < 4; ++turn)
    {
      const double score = (x * vectors[turn][0] + z * vectors[turn][1]) / length;
      if (score > best) { best = score; part.quarterTurn = turn; }
    }
    part.requiresUniqueOrientation = best < 0.99999;
  }
};

void hashPoint(uint64_t &hash, const rewrite::Position &point, const Piece &piece)
{
  double a, b, c;
  piece.local(point, a, b, c);
  mix(hash, static_cast<uint64_t>(micron(a)));
  mix(hash, static_cast<uint64_t>(micron(b)));
  mix(hash, static_cast<uint64_t>(micron(c)));
}

void hashRing(uint64_t &hash, const std::vector<rewrite::Position> &ring, const Piece &piece)
{
  mix(hash, ring.size());
  for (const auto &point : ring) hashPoint(hash, point, piece);
}

uint64_t pieceGeometryKey(const Piece &piece, const Skeleton &skeleton)
{
  uint64_t key = 14695981039346656037ull;
  mix(key, 1); // Recipe version.
  mix(key, static_cast<uint8_t>(piece.kind));
  mix(key, static_cast<uint8_t>(piece.faceKind));
  mix(key, static_cast<uint64_t>(micron(piece.width)));
  mix(key, static_cast<uint64_t>(micron(piece.height)));
  mix(key, static_cast<uint64_t>(micron(piece.depth)));
  mix(key, piece.faces.size());
  for (const uint32_t faceIndex : piece.faces)
  {
    const auto &face = skeleton.structure.faces[faceIndex];
    mix(key, static_cast<uint8_t>(face.kind));
    mix(key, static_cast<uint8_t>(face.aperture));
    mix(key, static_cast<uint64_t>(micron(face.archSpring)));
    mix(key, static_cast<uint64_t>(micron(face.thickness)));
    mix(key, static_cast<uint64_t>(micron(face.low)));
    mix(key, static_cast<uint64_t>(micron(face.high)));
    mix(key, static_cast<uint64_t>(micron(face.along0)));
    mix(key, static_cast<uint64_t>(micron(face.along1)));
    mix(key, face.detailedOpening);
    hashRing(key, face.outline, piece);
    hashRing(key, face.apertureBoundary, piece);
    mix(key, face.wallPanels.size());
    for (const auto &panel : face.wallPanels) hashRing(key, panel, piece);
    mix(key, face.openingReveals.size());
    for (const auto &reveal : face.openingReveals) hashRing(key, reveal, piece);
  }
  return key;
}

uint64_t pieceAppearanceKey(const Piece &piece, const Skeleton &skeleton)
{
  uint64_t key = 14695981039346656037ull;
  mix(key, 1);
  for (const uint32_t faceIndex : piece.faces)
    mix(key, static_cast<uint8_t>(skeleton.structure.faces[faceIndex].material));
  return key;
}

uint64_t facadeGeometryKey(const FacadeInstance &facade)
{
  uint64_t key = 14695981039346656037ull;
  mix(key, 2);
  mix(key, static_cast<uint8_t>(facade.asset));
  mix(key, static_cast<uint8_t>(facade.shape));
  mix(key, static_cast<uint64_t>(micron(facade.archSpring)));
  mix(key, static_cast<uint64_t>(micron(facade.width)));
  mix(key, static_cast<uint64_t>(micron(facade.height)));
  mix(key, static_cast<uint64_t>(micron(facade.depth)));
  return key;
}

uint64_t faceGeometryKey(const StructureFace &face, const LatticeFrame &frame)
{
  uint64_t key = 14695981039346656037ull;
  mix(key, 3);
  mix(key, static_cast<uint8_t>(face.kind));
  const auto origin = face.outline.empty() ? rewrite::Position{} : face.outline.front();
  const auto localOrigin = frame.local(origin);
  const auto ring = [&](const std::vector<rewrite::Position> &points) {
    mix(key, points.size());
    for (const auto &point : points) {
      const auto p = frame.local(point);
      for (unsigned axis = 0; axis < 3; ++axis)
        mix(key, static_cast<uint64_t>(micron(p[axis] - localOrigin[axis])));
    }
  };
  mix(key, static_cast<uint8_t>(face.aperture));
  mix(key, static_cast<uint64_t>(micron(face.archSpring)));
  mix(key, static_cast<uint64_t>(micron(face.thickness)));
  mix(key, static_cast<uint64_t>(micron(face.low)));
  mix(key, static_cast<uint64_t>(micron(face.high)));
  mix(key, static_cast<uint64_t>(micron(face.along0)));
  mix(key, static_cast<uint64_t>(micron(face.along1)));
  mix(key, face.detailedOpening);
  ring(face.outline); ring(face.apertureBoundary);
  mix(key, face.wallPanels.size());
  for (const auto &panel : face.wallPanels) ring(panel);
  mix(key, face.openingReveals.size());
  for (const auto &reveal : face.openingReveals) ring(reveal);
  return key;
}

uint64_t variationSeed(uint64_t dwelling, uint64_t seed, DwellingVoxelPartSource source, uint32_t index)
{
  uint64_t key = 14695981039346656037ull;
  mix(key, dwelling); mix(key, seed); mix(key, static_cast<uint8_t>(source)); mix(key, index);
  return key;
}

uint16_t occupancyPriority(PieceKind kind)
{
  switch (kind) {
    case PieceKind::Wall: return 10;
    case PieceKind::Floor:
    case PieceKind::Paving:
    case PieceKind::Deck:
    case PieceKind::Landing: return 20;
    case PieceKind::Roof:
    case PieceKind::Dormer: return 30;
    case PieceKind::Timber:
    case PieceKind::Post:
    case PieceKind::Pier:
    case PieceKind::Furniture: return 40;
    default: return 20;
  }
}

uint16_t occupancyPriority(FaceKind kind)
{
  switch(kind) {
    case FaceKind::Wall:
    case FaceKind::WindowedWall:
    case FaceKind::DoorWall:
    case FaceKind::DoubleDoorWall:
    case FaceKind::ArchWall: return 10;
    case FaceKind::Roof:
    case FaceKind::DormerRoof:
    case FaceKind::Cap: return 30;
    case FaceKind::Timber:
    case FaceKind::Post:
    case FaceKind::Pier:
    case FaceKind::Furniture: return 40;
    default: return 20;
  }
}

uint32_t portalPairCanonical(const Skeleton &skeleton,uint32_t sourceIndex)
{
  const auto &facade=skeleton.structure.instances[sourceIndex];
  if(facade.asset!=FacadeAsset::PortalFrame || facade.segment==UINT32_MAX)
    return UINT32_MAX;
  uint32_t canonical=sourceIndex;
  bool paired=false;
  for(uint32_t i=0;i<skeleton.structure.instances.size();++i) {
    const auto &other=skeleton.structure.instances[i];
    if(i!=sourceIndex && other.asset==facade.asset &&
       other.segment==facade.segment && other.supportFace!=facade.supportFace) {
      canonical=std::min(canonical,i);
      paired=true;
    }
  }
  return paired?canonical:UINT32_MAX;
}

} // namespace

uint16_t dwellingVoxelConnectorMask(PieceKind kind, FaceKind faceKind,
                                    PieceFace face, ContactKind contact)
{
  uint16_t mask = 0;
  switch (kind) {
    case PieceKind::Wall: mask = VoxelWall; break;
    case PieceKind::Floor:
    case PieceKind::Paving:
    case PieceKind::Deck:
    case PieceKind::Landing: mask = VoxelSlab; break;
    case PieceKind::Roof: mask = VoxelRoof; break;
    case PieceKind::Dormer:
      mask = faceKind == FaceKind::DormerWall || faceKind == FaceKind::DormerWindow ? VoxelWall :
             faceKind == FaceKind::DormerOpening ? VoxelOpening :
             faceKind == FaceKind::DormerCeiling ? VoxelSlab : VoxelRoof;
      break;
    case PieceKind::Ramp: mask = VoxelStair; break;
    case PieceKind::Pier:
    case PieceKind::Post:
    case PieceKind::Timber:
    case PieceKind::Furniture: mask = VoxelPropSocket; break;
    default: break;
  }
  if (face == PieceFace::Outside) mask |= VoxelOutside;
  if (face == PieceFace::Up || face == PieceFace::Bottom || contact == ContactKind::Capped)
    mask |= VoxelCap;
  if (contact == ContactKind::Jamb) mask |= VoxelOpening;
  return mask;
}

uint16_t dwellingVoxelConnectorMask(PieceKind kind, PieceFace face, ContactKind contact)
{
  return dwellingVoxelConnectorMask(kind,FaceKind::Count,face,contact);
}

bool buildDwellingVoxelAssembly(const generator::CellHouse &house, const rewrite::DwellingPlan &plan,
                                const Skeleton &skeleton, uint64_t dwellingId, uint64_t seed,
                                DwellingVoxelAssembly &result, std::string *error)
{
  if (!std::isfinite(house.frame.cellMetres) || house.frame.cellMetres <= 0 ||
      !std::isfinite(house.frame.originX) || !std::isfinite(house.frame.originZ) ||
      !std::isfinite(house.frame.axisX) || !std::isfinite(house.frame.axisZ) ||
      std::abs(std::hypot(house.frame.axisX, house.frame.axisZ) - 1.0) > 1e-5 ||
      house.partyEdges.size() != house.partyGaps.size())
    return fail(error, "Invalid dwelling voxel grid frame");
  const double ideal = house.frame.cellMetres / 0.1;
  if (!std::isfinite(ideal) || ideal < 1 || ideal > 10000)
    return fail(error, "Dwelling cell pitch cannot use the 10 cm policy");
  const uint32_t subdivisions = std::max(1u, static_cast<uint32_t>(std::llround(ideal)));
  const double pitch = house.frame.cellMetres / subdivisions;
  if (skeleton.pieceOfFace.size() != skeleton.structure.faces.size())
    return fail(error, "Skeleton face ownership is incomplete");

  DwellingVoxelAssembly out;
  out.dwellingId = dwellingId;
  out.seed = seed;
  out.grid.frame = house.frame;
  out.grid.columns = house.grid.width();
  out.grid.rows = house.grid.height();
  out.grid.voxelsPerCell = subdivisions;
  out.grid.voxelMetres = pitch;
  out.grid.partyEdges = house.partyEdges;
  out.grid.partyGaps = house.partyGaps;
  out.sourcePieceCount = static_cast<uint32_t>(skeleton.pieces.size());
  out.sourceFacadeCount = static_cast<uint32_t>(skeleton.structure.instances.size());
  out.sourceFaceCount = static_cast<uint32_t>(skeleton.structure.faces.size());
  out.sourceRoomCount = static_cast<uint32_t>(skeleton.rooms.size());
  out.sourceRoomConnectionCount = static_cast<uint32_t>(skeleton.roomConnections.size());
  out.plannedRoomCount = static_cast<uint32_t>(plan.rooms.size());
  out.plannedOpeningCount = static_cast<uint32_t>(plan.openings.size());
  for (const auto &opening : plan.openings)
    if (opening.exterior && opening.kind == rewrite::OpeningKind::Window)
      ++out.plannedExteriorWindowCount;
  out.plannedStairCount = static_cast<uint32_t>(plan.stairs.size());
  out.plannedRoofCount = static_cast<uint32_t>(plan.roofs.size());
  out.architecturalAgeYears = plan.architecturalAgeYears;
  out.weathering = plan.weathering;
  for (const auto &room : skeleton.rooms)
    if (!room.outdoor && room.blueprintRoom >= plan.rooms.size())
      return fail(error,"Skeleton room has no blueprint owner");
  out.parts.reserve(skeleton.pieces.size() + skeleton.structure.instances.size());
  out.sourceFaces.reserve(skeleton.structure.faces.size());
  out.connectors.reserve(skeleton.contacts.size());
  out.openings.reserve(plan.openings.size());
  out.roomPaths.reserve(skeleton.roomConnections.size());

  double ox, oz, ux, uz, vx, vz;
  geom2::framePoint(house.frame, 0, 0, ox, oz);
  geom2::framePoint(house.frame, 1, 0, ux, uz);
  geom2::framePoint(house.frame, 0, 1, vx, vz);
  LatticeFrame frame{{ox, 0, oz}, {(ux - ox) / house.frame.cellMetres, 0, (uz - oz) / house.frame.cellMetres},
                     {(vx - ox) / house.frame.cellMetres, 0, (vz - oz) / house.frame.cellMetres}, pitch};
  if (std::abs(frame.u.x * frame.v.z - frame.u.z * frame.v.x) < 1e-8)
    return fail(error, "Degenerate dwelling voxel grid axes");

  bool hasBounds = false;
  const auto finiteMetric = [](double value) { return std::isfinite(value) && std::abs(value) < 1e9; };
  const auto includePoint = [&](const rewrite::Position &point) {
    const auto local = frame.local(point);
    for (unsigned axis = 0; axis < 3; ++axis) {
      const double q = local[axis] / pitch;
      if (!std::isfinite(q) || q < double(std::numeric_limits<int32_t>::min()) + 1 ||
          q > double(std::numeric_limits<int32_t>::max()) - 2) return false;
      const int32_t low = static_cast<int32_t>(std::floor(q));
      const int32_t high = static_cast<int32_t>(std::ceil(q)) + 1;
      if (!hasBounds) { out.grid.minimumVoxel[axis] = low; out.grid.maximumVoxel[axis] = high; }
      else { out.grid.minimumVoxel[axis] = std::min(out.grid.minimumVoxel[axis],low);
             out.grid.maximumVoxel[axis] = std::max(out.grid.maximumVoxel[axis],high); }
    }
    hasBounds = true;
    return true;
  };
  for (const auto &face : skeleton.structure.faces) {
    for (const double value : {face.archSpring,face.thickness,face.low,face.high,
                               face.along0,face.along1})
      if (!finiteMetric(value)) return fail(error,"Invalid structure face metric");
    for (const auto &point : face.outline) if (!includePoint(point)) return fail(error,"Face bounds exceed voxel grid range");
    for (const auto &point : face.apertureBoundary) if (!includePoint(point)) return fail(error,"Aperture bounds exceed voxel grid range");
    for (const auto &ring : face.wallPanels) for (const auto &point : ring)
      if (!includePoint(point)) return fail(error,"Panel bounds exceed voxel grid range");
    for (const auto &ring : face.openingReveals) for (const auto &point : ring)
      if (!includePoint(point)) return fail(error,"Reveal bounds exceed voxel grid range");
  }
  for (const auto &facade : skeleton.structure.instances) {
    for (const double value : {facade.archSpring,facade.width,facade.height,facade.depth,
                               facade.u.x,facade.u.y,facade.u.z,facade.v.x,facade.v.y,
                               facade.v.z,facade.normal.x,facade.normal.y,facade.normal.z})
      if (!finiteMetric(value)) return fail(error,"Invalid facade metric");
    for (unsigned u = 0; u < 2; ++u) for (unsigned v = 0; v < 2; ++v) for (unsigned d = 0; d < 2; ++d) {
      const rewrite::Position p{facade.origin.x + facade.u.x * facade.width * u + facade.v.x * facade.height * v + facade.normal.x * facade.depth * d,
        facade.origin.y + facade.u.y * facade.width * u + facade.v.y * facade.height * v + facade.normal.y * facade.depth * d,
        facade.origin.z + facade.u.z * facade.width * u + facade.v.z * facade.height * v + facade.normal.z * facade.depth * d};
      if (!includePoint(p)) return fail(error,"Facade bounds exceed voxel grid range");
    }
  }
  if (!hasBounds) return fail(error,"Dwelling skeleton has no voxel geometry bounds");

  const auto notePlacement = [&](DwellingVoxelPart &part)
  {
    bool offGrid = false;
    for (const double residual : part.residualMetres)
      if (std::abs(residual) > 1e-6) { offGrid = true; break; }
    if (offGrid) ++out.unalignedPlacementCount;
    if (part.requiresUniqueOrientation) ++out.uniqueOrientationCount;
    part.requiresBakedGeometry = offGrid || part.requiresUniqueOrientation;
    if (part.requiresBakedGeometry) {
      // A snapped instance may still share its baked asset with another part
      // at the same sub-voxel offset and angle. Variation stays in appearance.
      for (const double residual : part.residualMetres)
        mix(part.geometryKey, static_cast<uint64_t>(micron(residual)));
      mix(part.geometryKey, static_cast<uint64_t>(micron(part.localAngleRadians)));
    }
  };
  for (uint32_t i = 0; i < skeleton.pieces.size(); ++i)
  {
    const auto &piece = skeleton.pieces[i];
    for (const double value : {piece.width,piece.height,piece.depth,piece.ux,piece.uz,
                               piece.wx,piece.wz,piece.origin.x,piece.origin.y,piece.origin.z})
      if (!finiteMetric(value)) return fail(error,"Invalid skeleton piece metric");
    for (const uint32_t face : piece.faces)
      if (face >= skeleton.structure.faces.size())
        return fail(error, "Skeleton piece contains invalid face index");
    DwellingVoxelPart part;
    part.source = DwellingVoxelPartSource::Piece;
    part.sourceIndex = i;
    part.sourceFaceFirst = static_cast<uint32_t>(out.sourceFaces.size());
    part.sourceFaceCount = static_cast<uint32_t>(piece.faces.size());
    part.segment = piece.segment;
    part.partyWall = piece.party;
    part.occupancyPriority = occupancyPriority(piece.kind);
    part.omitWhenCovered = piece.kind!=PieceKind::Furniture;
    part.geometryKey = pieceGeometryKey(piece, skeleton);
    part.appearanceKey = pieceAppearanceKey(piece, skeleton);
    part.variationSeed = variationSeed(dwellingId, seed, part.source, i);
    if (!frame.place(piece.origin, part)) return fail(error, "Piece placement exceeds voxel grid range");
    frame.orientation(piece.ux, piece.uz, part);
    notePlacement(part);
    out.sourceFaces.insert(out.sourceFaces.end(), piece.faces.begin(), piece.faces.end());
    out.parts.push_back(part);
  }
  for (uint32_t i = 0; i < skeleton.structure.instances.size(); ++i)
  {
    const auto &facade = skeleton.structure.instances[i];
    const uint32_t portalCanonical=portalPairCanonical(skeleton,i);
    const bool pairedPortal=portalCanonical!=UINT32_MAX;
    DwellingVoxelPart part;
    part.source = DwellingVoxelPartSource::Facade;
    part.connectorMask = VoxelPropSocket;
    part.occupancyPriority = pairedPortal && portalCanonical==i ? 51 : 50;
    part.doorLeafSurface=facade.asset==FacadeAsset::DoorLeaf ||
        facade.asset==FacadeAsset::DoorLeafLeft || facade.asset==FacadeAsset::DoorLeafRight;
    part.appearanceOverlay=facade.asset==FacadeAsset::WallBlock && facade.wooden &&
        std::min({facade.width,facade.height,facade.depth})<out.grid.voxelMetres;
    // Two skins of one portal are one movable frame assembly. At 10 cm a
    // thin wall may quantize both skins to identical cells; keep the first
    // source skin and count a fully covered partner.
    part.omitWhenCovered = (pairedPortal && portalCanonical!=i) || part.appearanceOverlay;
    part.omitFromOpaqueScene = facade.asset == FacadeAsset::Glass;
    part.sourceIndex = i;
    part.sourceFaceFirst = static_cast<uint32_t>(out.sourceFaces.size());
    part.supportPiece = facade.supportFace < skeleton.pieceOfFace.size()
                            ? skeleton.pieceOfFace[facade.supportFace] : UINT32_MAX;
    part.segment = facade.segment;
    part.geometryKey = facadeGeometryKey(facade);
    part.appearanceKey = 14695981039346656037ull;
    mix(part.appearanceKey, facade.wooden);
    mix(part.appearanceKey, facade.inside);
    part.variationSeed = variationSeed(dwellingId, seed, part.source, i);
    if (!frame.place(facade.origin, part)) return fail(error, "Facade placement exceeds voxel grid range");
    frame.orientation(facade.u.x, facade.u.z, part);
    notePlacement(part);
    out.parts.push_back(part);
  }
  for (uint32_t i = 0; i < skeleton.structure.faces.size(); ++i)
  {
    if (skeleton.pieceOfFace[i] != UINT32_MAX) continue;
    const auto &face = skeleton.structure.faces[i];
    DwellingVoxelPart part;
    part.source = DwellingVoxelPartSource::UnassignedFace;
    part.sourceIndex = i;
    part.sourceFaceFirst = static_cast<uint32_t>(out.sourceFaces.size());
    part.sourceFaceCount = 1;
    part.segment = face.segment;
    part.partyWall = face.party;
    part.occupancyPriority = occupancyPriority(face.kind);
    part.omitWhenCovered = true; // Unassigned structural face, never movable facade joinery.
    part.geometryKey = faceGeometryKey(face, frame);
    part.appearanceKey = 14695981039346656037ull;
    mix(part.appearanceKey, static_cast<uint8_t>(face.material));
    part.variationSeed = variationSeed(dwellingId, seed, part.source, i);
    if (!frame.place(face.outline.empty() ? rewrite::Position{} : face.outline.front(), part))
      return fail(error, "Unassigned face placement exceeds voxel grid range");
    part.requiresUniqueOrientation = true;
    notePlacement(part);
    out.sourceFaces.push_back(i);
    out.parts.push_back(part);
  }
  for (const auto &contact : skeleton.contacts)
  {
    if (contact.a >= skeleton.pieces.size() ||
        (contact.b != UINT32_MAX && contact.b >= skeleton.pieces.size()))
      return fail(error, "Skeleton contact has invalid piece index");
    DwellingVoxelConnector connector;
    connector.pieceA = contact.a; connector.pieceB = contact.b;
    connector.kind = contact.kind; connector.faceA = contact.faceOfA; connector.faceB = contact.faceOfB;
    connector.localA={contact.ua,contact.va,contact.wa};
    connector.localB={contact.ub,contact.vb,contact.wb};
    connector.maskA=dwellingVoxelConnectorMask(skeleton.pieces[contact.a].kind,
      skeleton.pieces[contact.a].faceKind,contact.faceOfA,contact.kind);
    if (contact.b != UINT32_MAX)
      connector.maskB=dwellingVoxelConnectorMask(skeleton.pieces[contact.b].kind,
        skeleton.pieces[contact.b].faceKind,contact.faceOfB,contact.kind);
    if (!frame.place(contact.at, connector)) return fail(error, "Contact exceeds voxel grid range");
    out.connectors.push_back(connector);
  }
  for (uint32_t i = 0; i < plan.openings.size(); ++i)
  {
    const auto &opening = plan.openings[i];
    DwellingVoxelPart a, b;
    if (!frame.place(opening.a,a) || !frame.place(opening.b,b))
      return fail(error,"Opening exceeds voxel grid range");
    DwellingVoxelOpening record;
    record.sourceIndex=i; record.floor=opening.floor;
    record.wall=opening.wall; record.room=opening.room;
    record.kind=opening.kind; record.exterior=opening.exterior;
    record.startVoxel=a.gridPosition;record.endVoxel=b.gridPosition;
    record.startResidual=a.residualMetres;record.endResidual=b.residualMetres;
    record.base=opening.base;record.top=opening.top;
    record.sill=opening.sill;record.head=opening.head;
    out.openings.push_back(record);
  }
  for (uint32_t i = 0; i < skeleton.roomConnections.size(); ++i)
  {
    const auto &connection = skeleton.roomConnections[i];
    if ((connection.a != UINT32_MAX && connection.a >= skeleton.rooms.size()) ||
        (connection.b != UINT32_MAX && connection.b >= skeleton.rooms.size()))
      return fail(error,"Room path references an invalid room");
    DwellingVoxelPart placed;
    if (!frame.place(connection.at,placed)) return fail(error,"Room path exceeds voxel grid range");
    DwellingVoxelRoomPath path;
    path.sourceIndex=i;path.roomA=connection.a;path.roomB=connection.b;
    path.kind=connection.kind;path.gridPosition=placed.gridPosition;
    path.residualMetres=placed.residualMetres;
    out.roomPaths.push_back(path);
  }
  if (!auditDwellingVoxelAssembly(out, skeleton, error)) return false;
  result = std::move(out);
  return true;
}

bool auditDwellingVoxelAssembly(const DwellingVoxelAssembly &assembly, const Skeleton &skeleton, std::string *error)
{
  if (assembly.sourcePieceCount != skeleton.pieces.size() ||
      assembly.sourceFacadeCount != skeleton.structure.instances.size() ||
      assembly.sourceFaceCount != skeleton.structure.faces.size() ||
      assembly.sourceRoomCount != skeleton.rooms.size() ||
      assembly.sourceRoomConnectionCount != skeleton.roomConnections.size() ||
      assembly.connectors.size() != skeleton.contacts.size() ||
      assembly.roomPaths.size() != skeleton.roomConnections.size() ||
      assembly.openings.size() != assembly.plannedOpeningCount)
    return fail(error, "Dwelling voxel assembly source counts diverged");
  for (uint32_t i = 0; i < assembly.roomPaths.size(); ++i) {
    const auto &path=assembly.roomPaths[i];
    const auto &source=skeleton.roomConnections[i];
    if(path.sourceIndex!=i || path.roomA!=source.a || path.roomB!=source.b || path.kind!=source.kind)
      return fail(error,"Room path identity diverged");
  }
  for (uint32_t i = 0; i < assembly.openings.size(); ++i)
    if (assembly.openings[i].sourceIndex!=i || assembly.openings[i].mask!=VoxelOpening)
      return fail(error,"Opening identity diverged");
  for (uint32_t i = 0; i < assembly.connectors.size(); ++i) {
    const auto &connector=assembly.connectors[i];
    const auto &source=skeleton.contacts[i];
    if(connector.pieceA!=source.a || connector.pieceB!=source.b ||
       connector.kind!=source.kind || connector.faceA!=source.faceOfA ||
       connector.faceB!=source.faceOfB ||
       connector.localA!=std::array<double,3>{source.ua,source.va,source.wa} ||
       connector.localB!=std::array<double,3>{source.ub,source.vb,source.wb} ||
       connector.maskA!=dwellingVoxelConnectorMask(skeleton.pieces[source.a].kind,
         skeleton.pieces[source.a].faceKind,source.faceOfA,source.kind) ||
       connector.maskB!=(source.b==UINT32_MAX ? 0 :
         dwellingVoxelConnectorMask(skeleton.pieces[source.b].kind,
           skeleton.pieces[source.b].faceKind,source.faceOfB,source.kind)))
      return fail(error,"Piece connector identity diverged");
  }
  for (unsigned axis = 0; axis < 3; ++axis)
    if (assembly.grid.minimumVoxel[axis] >= assembly.grid.maximumVoxel[axis])
      return fail(error,"Invalid dwelling voxel bounds");
  std::vector<uint8_t> pieces(skeleton.pieces.size(), 0), facades(skeleton.structure.instances.size(), 0);
  std::vector<uint8_t> faces(skeleton.structure.faces.size(), 0);
  for (const auto &part : assembly.parts)
  {
    if (!part.geometryKey || !part.appearanceKey ||
        uint64_t(part.sourceFaceFirst) + part.sourceFaceCount > assembly.sourceFaces.size())
      return fail(error, "Invalid dwelling voxel part record");
    if (part.source == DwellingVoxelPartSource::Piece)
    {
      if(part.appearanceOverlay || part.doorLeafSurface)
        return fail(error,"Structural piece has facade appearance policy");
      if (part.sourceIndex >= pieces.size() || pieces[part.sourceIndex]++)
        return fail(error, "Missing or duplicate skeleton piece");
      if (part.sourceFaceCount != skeleton.pieces[part.sourceIndex].faces.size())
        return fail(error, "Piece face count diverged");
      if (part.partyWall != skeleton.pieces[part.sourceIndex].party)
        return fail(error, "Piece party wall identity diverged");
      if (part.occupancyPriority != occupancyPriority(skeleton.pieces[part.sourceIndex].kind))
        return fail(error, "Piece occupancy priority diverged");
      if (part.omitWhenCovered != (skeleton.pieces[part.sourceIndex].kind!=PieceKind::Furniture))
        return fail(error, "Piece covered-surface policy diverged");
      if (part.omitFromOpaqueScene)
        return fail(error, "Structural piece was omitted from the opaque scene");
    }
    else if (part.source == DwellingVoxelPartSource::Facade)
    {
      if (part.sourceIndex >= facades.size() || facades[part.sourceIndex]++ || part.sourceFaceCount)
        return fail(error, "Missing or duplicate facade instance");
      const auto &facade=skeleton.structure.instances[part.sourceIndex];
      const uint32_t portalCanonical=portalPairCanonical(skeleton,part.sourceIndex);
      const bool pairedPortal=portalCanonical!=UINT32_MAX;
      const bool leaf=facade.asset==FacadeAsset::DoorLeaf ||
          facade.asset==FacadeAsset::DoorLeafLeft || facade.asset==FacadeAsset::DoorLeafRight;
      const bool overlay=facade.asset==FacadeAsset::WallBlock && facade.wooden &&
          std::min({facade.width,facade.height,facade.depth})<assembly.grid.voxelMetres;
      if(part.doorLeafSurface!=leaf || part.appearanceOverlay!=overlay ||
         part.omitWhenCovered!=((pairedPortal && portalCanonical!=part.sourceIndex)||overlay))
        return fail(error,"Facade paired-skin policy diverged");
      const auto support=facade.supportFace;
      const uint32_t expected=support<skeleton.pieceOfFace.size()?skeleton.pieceOfFace[support]:UINT32_MAX;
      if(part.supportPiece!=expected)return fail(error,"Facade support piece diverged");
      if(part.connectorMask!=VoxelPropSocket)return fail(error,"Facade prop socket diverged");
      if(part.occupancyPriority!=(pairedPortal && portalCanonical==part.sourceIndex ? 51 : 50))
        return fail(error,"Facade occupancy priority diverged");
      if(part.omitFromOpaqueScene != (skeleton.structure.instances[part.sourceIndex].asset==FacadeAsset::Glass))
        return fail(error,"Facade opacity policy diverged");
    }
    else if (part.source == DwellingVoxelPartSource::UnassignedFace)
    {
      if(part.appearanceOverlay || part.doorLeafSurface)
        return fail(error,"Unassigned face has facade appearance policy");
      if(!part.omitWhenCovered)return fail(error,"Unassigned structural face lost covered-surface policy");
      if (part.sourceIndex >= faces.size() || part.sourceFaceCount != 1 ||
          skeleton.pieceOfFace[part.sourceIndex] != UINT32_MAX)
        return fail(error, "Invalid unassigned structure face");
      if (part.partyWall != skeleton.structure.faces[part.sourceIndex].party)
        return fail(error, "Unassigned party wall identity diverged");
      if (part.occupancyPriority != occupancyPriority(skeleton.structure.faces[part.sourceIndex].kind))
        return fail(error, "Unassigned face occupancy priority diverged");
      if (part.omitFromOpaqueScene)
        return fail(error, "Structure face was omitted from the opaque scene");
    }
    else return fail(error, "Unknown voxel part source");
    for (uint32_t j = 0; j < part.sourceFaceCount; ++j)
    {
      const uint32_t face = assembly.sourceFaces[part.sourceFaceFirst + j];
      if (face >= faces.size() || faces[face]++) return fail(error, "Missing or duplicate structure face");
      if (part.source == DwellingVoxelPartSource::Piece &&
          skeleton.pieces[part.sourceIndex].faces[j] != face)
        return fail(error, "Piece face identity diverged");
      if (part.source == DwellingVoxelPartSource::Piece &&
          skeleton.pieceOfFace[face] != part.sourceIndex)
        return fail(error, "Piece face ownership diverged");
      if (part.source == DwellingVoxelPartSource::UnassignedFace && face != part.sourceIndex)
        return fail(error,"Unassigned face identity diverged");
    }
  }
  if (std::find(pieces.begin(), pieces.end(), 0) != pieces.end() ||
      std::find(facades.begin(), facades.end(), 0) != facades.end() ||
      std::find(faces.begin(), faces.end(), 0) != faces.end())
    return fail(error, "Dwelling voxel assembly omitted a source item");
  return true;
}

} // namespace procedural::world::hierarchy
