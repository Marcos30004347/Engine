#include "procedural/world/hierarchy/DwellingVoxelAssembly.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"

#include <cmath>
#include <iostream>
#include <string>

using namespace procedural;
using namespace procedural::world;

namespace {

int failures = 0;
void check(bool condition, const char *message)
{
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

rewrite::Position point(const geom2::CellFrame &frame, double u, double v, double y = 0)
{
  double x = 0, z = 0;
  geom2::framePoint(frame, u, v, x, z);
  return {x, y, z};
}

void assemblyCoverageAndKeys()
{
  generator::CellHouse house;
  house.grid = geom2::CellGrid(4, 4);
  house.frame = {2.2, 10, 20, 0, 1};
  rewrite::DwellingPlan plan;
  rewrite::DwellingOpening opening;
  opening.a=point(house.frame,1.2,1,.5);
  opening.b=point(house.frame,1.8,1,.5);
  opening.wall=9;opening.room=4;
  opening.kind=rewrite::OpeningKind::Window;opening.exterior=true;
  plan.openings.push_back(opening);
  plan.rooms.emplace_back();
  hierarchy::Skeleton skeleton;

  hierarchy::StructureFace wall;
  wall.kind = hierarchy::FaceKind::Wall;
  wall.material = hierarchy::Material::Stone;
  wall.outline = {point(house.frame,1,1),point(house.frame,2,1),
                  point(house.frame,2,1,2),point(house.frame,1,1,2)};
  skeleton.structure.faces.push_back(wall);

  hierarchy::StructureFace loose = wall;
  loose.kind = hierarchy::FaceKind::Paving;
  loose.outline = {point(house.frame,0,0),point(house.frame,1,0),
                   point(house.frame,1,1),point(house.frame,0,1)};
  skeleton.structure.faces.push_back(loose);
  skeleton.pieceOfFace = {0, UINT32_MAX};

  hierarchy::Piece piece;
  piece.kind = hierarchy::PieceKind::Wall;
  piece.origin = point(house.frame,1,1);
  piece.ux = 0; piece.uz = 1;
  piece.wx = -1; piece.wz = 0;
  piece.width = 2.2; piece.height = 2; piece.depth = .3;
  piece.faces = {0};
  skeleton.pieces.push_back(piece);

  hierarchy::FacadeInstance facade;
  facade.origin = point(house.frame,1.5,1,.5);
  facade.u = {0,0,1};
  facade.supportFace = 0;
  skeleton.structure.instances.push_back(facade);

  hierarchy::Contact contact;
  contact.a = 0;
  contact.b = UINT32_MAX;
  contact.kind = hierarchy::ContactKind::Capped;
  contact.at = point(house.frame,1,1,2);
  contact.ua=1.1;contact.va=2;
  skeleton.contacts.push_back(contact);
  hierarchy::RoomNode room;
  room.id=4;room.blueprintRoom=0;
  skeleton.rooms.push_back(room);
  hierarchy::RoomConnection path;
  path.a=0;path.b=UINT32_MAX;path.at=point(house.frame,1.5,1,.5);
  skeleton.roomConnections.push_back(path);

  hierarchy::DwellingVoxelAssembly first, second;
  std::string error;
  check(hierarchy::buildDwellingVoxelAssembly(house,plan,skeleton,42,77,first,&error),"build assembly");
  if (first.parts.empty()) { std::cerr << error << '\n'; return; }
  check(first.grid.voxelsPerCell == 22 && std::abs(first.grid.voxelMetres-.1)<1e-9,
        "10 cm subdivision");
  check(first.parts.size()==3 && first.sourceFaces.size()==2 && first.connectors.size()==1,
        "piece, facade, unique face and connector coverage");
  check(first.openings.size()==1 && first.openings[0].wall==9 &&
        first.roomPaths.size()==1 && first.roomPaths[0].roomA==0 &&
        first.plannedExteriorWindowCount==1,
        "opening and room path identities");
  check(first.connectors[0].localA[0]==1.1 && first.connectors[0].localA[1]==2,
        "contact coordinates retained in piece frame");
  check(first.parts[0].gridPosition==std::array<int32_t,3>{22,0,22},"rotated integer placement");
  check(first.parts[1].supportPiece==0,"facade source ownership");
  check(first.parts[1].connectorMask==hierarchy::VoxelPropSocket,"facade prop socket");
  check(first.parts[1].occupancyPriority>first.parts[0].occupancyPriority,
        "opening joinery owns cells shared with wall pieces");
  check(first.unalignedPlacementCount==0,"aligned placements");
  check(hierarchy::auditDwellingVoxelAssembly(first,skeleton,&error),"audit assembly");

  check(hierarchy::buildDwellingVoxelAssembly(house,plan,skeleton,42,77,second,&error),
        "deterministic rebuild");
  check(first.parts[0].geometryKey==second.parts[0].geometryKey &&
        first.parts[0].appearanceKey==second.parts[0].appearanceKey &&
        first.parts[0].variationSeed==second.parts[0].variationSeed,"stable keys and seed");

  skeleton.structure.faces[0].material = hierarchy::Material::Timber;
  check(hierarchy::buildDwellingVoxelAssembly(house,plan,skeleton,42,77,second,&error),
        "material variant build");
  check(first.parts[0].geometryKey==second.parts[0].geometryKey &&
        first.parts[0].appearanceKey!=second.parts[0].appearanceKey,
        "geometry deduplicates across materials");

  auto omitted = first;
  omitted.parts.pop_back();
  check(!hierarchy::auditDwellingVoxelAssembly(omitted,skeleton,&error),"omitted source rejected");

  skeleton.pieces[0].origin.x += .02;
  check(hierarchy::buildDwellingVoxelAssembly(house,plan,skeleton,42,77,second,&error) &&
        second.unalignedPlacementCount>0,"off-grid placement reported");
  auto third=second;
  check(hierarchy::buildDwellingVoxelAssembly(house,plan,skeleton,99,123,third,&error) &&
        second.parts[0].requiresBakedGeometry &&
        second.parts[0].geometryKey==third.parts[0].geometryKey &&
        second.parts[0].variationSeed!=third.parts[0].variationSeed,
        "baked offset geometry shares across dwelling seeds");

  skeleton.structure.instances[0].asset=hierarchy::FacadeAsset::PortalFrame;
  skeleton.structure.instances[0].segment=0;
  skeleton.structure.instances[0].inside=false;
  auto innerPortal=skeleton.structure.instances[0];
  innerPortal.inside=true;
  innerPortal.supportFace=1;
  skeleton.structure.instances.push_back(innerPortal);
  check(hierarchy::buildDwellingVoxelAssembly(house,plan,skeleton,99,123,third,&error) &&
        third.parts.size()==4 && third.parts[1].occupancyPriority==51 &&
        third.parts[2].occupancyPriority==50 && third.parts[2].omitWhenCovered &&
        !third.parts[1].omitWhenCovered &&
        hierarchy::auditDwellingVoxelAssembly(third,skeleton,&error),
        "paired portal skins retain one visible movable frame");
  skeleton.structure.instances[0].inside=true; // Both sides of an interior wall belong to rooms.
  check(hierarchy::buildDwellingVoxelAssembly(house,plan,skeleton,99,123,third,&error) &&
        third.parts[1].occupancyPriority==51 && third.parts[2].omitWhenCovered &&
        hierarchy::auditDwellingVoxelAssembly(third,skeleton,&error),
        "interior portal skins pair without an exterior face");
}

} // namespace

int main()
{
  assemblyCoverageAndKeys();
  return failures ? 1 : 0;
}
