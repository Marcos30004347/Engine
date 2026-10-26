#include "procedural/world/WorldCityVoxelBoundaries.hpp"
#include "procedural/world/hierarchy/DwellingVoxelAssembly.hpp"
#include "procedural/world/generator/CellStreet.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <tuple>

using namespace procedural;
using namespace procedural::world;
using namespace procedural::world::rewrite;

namespace {
int failures = 0;
void check(bool value, const char *message)
{
  if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

CitySurfacePiece surface(CitySurfaceKind kind, uint64_t owner, double x0, double z0, double x1, double z1)
{
  CitySurfacePiece piece;
  piece.kind=kind; piece.owner=owner;
  for (const auto &[x,z] : {std::pair{x0,z0}, {x1,z0}, {x1,z1}, {x0,z1}})
    piece.polygon.outer.push_back({geom2::toUnit(x),geom2::toUnit(z)});
  return piece;
}

void partyAndSurfaceOwnership()
{
  CitySurfaceStructure surfaces;
  surfaces.pieces.push_back(surface(CitySurfaceKind::Road,40,-32,-32,0,0));
  surfaces.pieces.push_back(surface(CitySurfaceKind::Sidewalk,41,0,-32,32,0));
  const std::array<int64_t,4> edge{0,0,0,8000};
  const std::vector<CityVoxelPartyLine> lines{{edge,8,3}};
  CityVoxelBoundaryPlan plan;
  std::string error;
  check(buildCityVoxelBoundaryPlan(surfaces,lines,{3,8},plan,&error),"build accepted party seam");
  check(plan.features.size()==2 && plan.features[0].kind==CityVoxelFeatureKind::Road &&
        plan.features[1].kind==CityVoxelFeatureKind::Paving,"surface semantic ownership");
  check(plan.features[0].firstTile.x==-1 && plan.features[0].lastTile.x==0 &&
        plan.features[1].firstTile.x==0 && plan.features[1].lastTile.x==1 &&
        plan.terrain.minTileX==-1 && plan.terrain.maxTileX==2,
        "exact tile-plane feature bounds include both clip candidates");
  check(plan.partySeams.size()==1 && plan.partySeams[0].shared &&
        plan.partySeams[0].faceOwner==3,"one deterministic party face owner");
  CityVoxelBoundaryPlan swapped;
  check(buildCityVoxelBoundaryPlan(surfaces,{{edge,3,8}},{8,3},swapped,&error) &&
        swapped.partySeams.size()==1 && swapped.partySeams[0].faceOwner==plan.partySeams[0].faceOwner &&
        swapped.partySeams[0].edgeMillimetres==plan.partySeams[0].edgeMillimetres,
        "party resolution independent of input order");
  check(buildCityVoxelBoundaryPlan(surfaces,lines,{8},swapped,&error) &&
        !swapped.partySeams[0].shared && swapped.partySeams[0].faceOwner==8,
        "surviving house retains its outside face");
  check(buildCityVoxelBoundaryPlan(surfaces,lines,{},swapped,&error) &&
        swapped.partySeams.empty(),"absent houses emit no shared face");
  auto broken=plan;
  broken.partySeams[0].faceOwner=8;
  check(!auditCityVoxelBoundaryPlan(broken,&error),"doubled party ownership rejected");

  hierarchy::Skeleton skeleton;
  hierarchy::StructureFace face;
  face.party=true;
  face.outline={{0,0,0},{2,0,0},{2,2,0},{0,2,0}};
  skeleton.structure.faces.push_back(face);
  hierarchy::Piece piece;
  piece.party=true;piece.outside=0;
  skeleton.pieces.push_back(piece);
  hierarchy::DwellingVoxelAssembly houseA,houseB;
  houseA.dwellingId=3;houseB.dwellingId=8;
  houseA.grid.voxelMetres=houseB.grid.voxelMetres=.1;
  hierarchy::DwellingVoxelPart part;
  part.partyWall=true;
  houseA.parts.push_back(part);houseB.parts.push_back(part);
  const std::vector<CityVoxelPartyLine> physicalLine{{{0,0,2000,0},3,8}};
  std::vector<CityVoxelPartyFacet> a,b;
  check(collectCityVoxelPartyFacets(houseA,skeleton,physicalLine,a,&error) && a.size()==1 &&
        collectCityVoxelPartyFacets(houseB,skeleton,physicalLine,b,&error) && b.size()==1,
        "authored party faces collected without keeping skeletons");
  CityVoxelBoundaryPlan paired;
  check(buildCityVoxelBoundaryPlan(surfaces,physicalLine,{3,8},paired,&error),"physical party plan");
  a.insert(a.end(),b.begin(),b.end());
  check(resolveCityVoxelPartyContacts(paired,a,&error) && paired.partyContacts.size()==1 &&
        paired.partyContacts[0].faceOwner==3 && paired.partyContacts[0].along0==0 &&
        paired.partyContacts[0].along1==2000 && paired.partyContacts[0].bottom==0 &&
        paired.partyContacts[0].top==2000,"overlapping wall strips have one face owner");
  auto doubled=a;
  doubled.push_back(doubled.front());
  check(!resolveCityVoxelPartyContacts(paired,doubled,&error),
        "duplicate coincident party contact rejected");
  for(auto &vertex:skeleton.structure.faces[0].outline)vertex.z=.08;
  check(collectCityVoxelPartyFacets(houseB,skeleton,physicalLine,b,&error) && b.size()==1,
        "nearby wall retains a candidate strip");
  std::vector<CityVoxelPartyFacet> nearby{a.front(),b.front()};
  check(resolveCityVoxelPartyContacts(paired,nearby,&error) && paired.partyContacts.empty(),
        "a real sub-voxel gap never loses its faces");
  skeleton.structure.faces[0].outline[0].z=.4;
  skeleton.structure.faces[0].outline[1].z=.4;
  skeleton.structure.faces[0].outline[2].z=.4;
  skeleton.structure.faces[0].outline[3].z=.4;
  check(collectCityVoxelPartyFacets(houseB,skeleton,physicalLine,b,&error) && b.empty(),
        "separated walls never suppress a party face");
}

void terrainBordersAndFoundations()
{
  const auto field=[](double x,double z) { return .31*x-.17*z+std::sin(x*.03+z*.07)*.4; };
  for (int32_t tileX=-3;tileX<=3;++tileX)
    for (int32_t tileZ=-3;tileZ<=3;++tileZ) {
      const int64_t x=int64_t(tileX)*CityVoxelTileGrid::voxelsPerTile;
      const int64_t z=int64_t(tileZ)*CityVoxelTileGrid::voxelsPerTile;
      check(cityVoxelTileOfCell(x,z).x==tileX && cityVoxelTileOfCell(x-1,z).x==tileX-1,
            "half-open horizontal tile ownership");
      check(cityVoxelTileOfCell(x,z).z==tileZ && cityVoxelTileOfCell(x,z-1).z==tileZ-1,
            "half-open vertical tile ownership");
      for (int32_t step=0;step<=CityVoxelTileGrid::voxelsPerTile;++step) {
        int64_t fromLeft=0,fromRight=0,fromBottom=0,fromTop=0;
        check(cityVoxelTerrainSample(x,z+step,field,fromLeft) &&
              cityVoxelTerrainSample((int64_t(tileX-1)+1)*CityVoxelTileGrid::voxelsPerTile,z+step,field,fromRight) &&
              fromLeft==fromRight,"adjacent tile X border samples match");
        check(cityVoxelTerrainSample(x+step,z,field,fromBottom) &&
              cityVoxelTerrainSample(x+step,(int64_t(tileZ-1)+1)*CityVoxelTileGrid::voxelsPerTile,field,fromTop) &&
              fromBottom==fromTop,"adjacent tile Z border samples match");
      }
    }
  const std::vector<Position> footprint{{-1,0,-1},{1,0,-1},{1,0,1},{-1,0,1}};
  std::vector<CityVoxelFoundationSample> samples;
  std::string error;
  check(sampleCityVoxelFoundationJoin(footprint,2.,field,samples,&error),"sample foundation perimeter");
  check(samples.size()>=80,"foundation samples at most 10 cm apart");
  for (const auto &sample:samples)
    check(sample.groundMillimetres<=sample.topMillimetres && sample.topMillimetres==2000,
          "foundation and terrain share a bounded join");
  check(!sampleCityVoxelFoundationJoin(footprint,-100.,field,samples,&error),
        "terrain penetration rejected");
}

void connectorSemantics()
{
  using namespace hierarchy;
  for (unsigned quarterTurn=0;quarterTurn<4;++quarterTurn)
    for (auto face:{PieceFace::Up,PieceFace::Bottom,PieceFace::Left,PieceFace::Right,
                   PieceFace::Outside,PieceFace::Inside}) {
      const auto wall=dwellingVoxelConnectorMask(PieceKind::Wall,face,ContactKind::Run);
      check((wall&VoxelWall)!=0,"rotated wall connector retained");
      check(bool(wall&VoxelCap)==(face==PieceFace::Up || face==PieceFace::Bottom),
            "wall cap survives all quarter turns");
      check((dwellingVoxelConnectorMask(PieceKind::Floor,face,ContactKind::Stands)&VoxelSlab)!=0,
            "rotated slab connector retained");
      check((dwellingVoxelConnectorMask(PieceKind::Roof,face,ContactKind::Capped)&VoxelRoof)!=0,
            "rotated roof connector retained");
      check((dwellingVoxelConnectorMask(PieceKind::Ramp,face,ContactKind::Arrives)&VoxelStair)!=0,
            "rotated stair connector retained");
      check((dwellingVoxelConnectorMask(PieceKind::Furniture,face,ContactKind::Meets)&VoxelPropSocket)!=0,
            "rotated prop socket retained");
      check((dwellingVoxelConnectorMask(PieceKind::Wall,face,ContactKind::Jamb)&VoxelOpening)!=0,
            "rotated opening connector retained");
      check((dwellingVoxelConnectorMask(PieceKind::Dormer,FaceKind::DormerWall,face,ContactKind::Meets)&VoxelWall)!=0 &&
            (dwellingVoxelConnectorMask(PieceKind::Dormer,FaceKind::DormerOpening,face,ContactKind::Meets)&VoxelOpening)!=0,
            "dormer wall and opening keep distinct connector semantics");
    }
}

void generatedStreetJoin()
{
  generator::CellStreetBrief brief;
  brief.id=71;brief.seed=71;
  brief.a={geom2::toUnit(0.),geom2::toUnit(0.)};
  brief.b={geom2::toUnit(38.),geom2::toUnit(0.)};
  brief.depthMetres=13.;
  brief.building=generator::BuildingClass::Dwelling;
  generator::CellDwellingSettings settings;
  generator::CellStreet street;
  generator::Ledger ledger;
  std::string error;
  check(generator::planCellStreet(brief,settings,street,ledger,&error),"plan adjacent street dwellings");
  if(street.plots.size()<2)return;
  std::vector<rewrite::DwellingPlan> plans;
  check(generator::realiseCellStreet(street,settings,plans,&error),"realise adjacent street dwellings");
  if(plans.size()!=street.plots.size())return;
  bool joined=false;
  for(size_t k=0;k+1<street.plots.size();++k) {
    const auto &left=street.plots[k],&right=street.plots[k+1];
    if(!left.planned || !right.planned)continue;
    const double lineX=double(right.fromCell)*street.cellMetres;
    const double length=double(std::min(left.depthCells,right.depthCells))*street.cellMetres;
    const CityVoxelPartyLine line{{std::llround(lineX*1000.),0,std::llround(lineX*1000.),
                                    std::llround(length*1000.)},101,102};
    std::vector<CityVoxelPartyFacet> strips;
    for(const auto [plot,plan,id]:{std::tuple{&left,&plans[k],uint64_t(101)},
                                    std::tuple{&right,&plans[k+1],uint64_t(102)}}) {
      const auto skeleton=hierarchy::buildSkeleton(*plan);
      hierarchy::DwellingVoxelAssembly assembly;
      if(!hierarchy::buildDwellingVoxelAssembly(plot->house,*plan,skeleton,id,id,assembly,&error)) {
        check(false,"build adjacent street assembly");return;
      }
      std::vector<CityVoxelPartyFacet> one;
      if(!collectCityVoxelPartyFacets(assembly,skeleton,{line},one,&error)) {
        std::cerr << "party facet error: " << error << '\n';
        check(false,"collect generated party wall strips");return;
      }
      strips.insert(strips.end(),one.begin(),one.end());
    }
    CitySurfaceStructure surfaces;
    surfaces.pieces.push_back(surface(CitySurfaceKind::Lot,101,0,0,38,13));
    CityVoxelBoundaryPlan boundary;
    check(buildCityVoxelBoundaryPlan(surfaces,{line},{101,102},boundary,&error) &&
          resolveCityVoxelPartyContacts(boundary,strips,&error),
          "resolve generated street boundary");
    check(!strips.empty(),"generated street exposes authored party strips");
    const bool a=std::any_of(strips.begin(),strips.end(),[](const auto &strip){return strip.building==101;});
    const bool b=std::any_of(strips.begin(),strips.end(),[](const auto &strip){return strip.building==102;});
    if(!(a && b))check(boundary.partyContacts.empty(),
                       "one-sided authored party wall keeps its visible face");
    joined=true;
    break;
  }
  check(joined,"street fixture contains adjacent planned dwellings");
}
}

int main()
{
  partyAndSurfaceOwnership();
  terrainBordersAndFoundations();
  connectorSemantics();
  generatedStreetJoin();
  return failures ? 1 : 0;
}
