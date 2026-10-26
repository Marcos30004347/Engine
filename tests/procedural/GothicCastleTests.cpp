#include "procedural/world/hierarchy/CellSkeleton.hpp"
#include "procedural/world/generator/CompoundPlanner.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include "procedural/world/generator/FortressJunction.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"
#include "procedural/world/generator/DwellingStylePlanner.hpp"
#include "procedural/world/WorldArchitecturalClearance.hpp"
#include "fixtures/HistoricalCastlePrecinct.hpp"
#include <cmath>
#include <iostream>
#include <set>
#include <map>
#include <array>
using namespace procedural::world;
using namespace procedural::world::rewrite;
int main(int argc,char **argv) {
  int failed=0;auto check=[&](bool ok,const char *message){std::cout<<(ok?"PASS ":"FAIL ")<<message<<'\n';failed+=!ok;};
  {
    using namespace generator;
    check(historicalDwellingStyle(0,10)==DwellingArchitecture::Vernacular && historicalDwellingStyle(400,10)==DwellingArchitecture::Roman && historicalDwellingStyle(400,12)==DwellingArchitecture::HighFantasy,"historical style uses construction age with regional variation");
    CellDwellingBrief brief;brief.id=10;brief.seed=41;brief.floors=1;brief.architecture=DwellingArchitecture::Roman;brief.architecturalAgeYears=400;
    brief.lot.outer={{0,0},{procedural::geom2::toUnit(32.),0},{procedural::geom2::toUnit(32.),procedural::geom2::toUnit(32.)},{0,procedural::geom2::toUnit(32.)}};
    brief.capacity=CellDwellingBrief::Capacity{760.,12,32.,true};
    CellDwellingSettings settings;CellHouse house;DwellingPlan p;Ledger ledger;std::string error;
    const bool ok=planCellDwelling(brief,settings,house,ledger,&error)&&realiseCellDwelling(house,settings,p,&error);
    if(!ok)std::cout<<error<<'\n';
    check(ok&&!house.courts.empty()&&!p.doors.empty()&&p.rooms.size()>2,"Roman blueprint reserves an accessible atrium before rooms and doors");
    check(ok&&p.colonnadeColumns>=4&&p.cofferBeams>0,"Roman atrium receives columns and interior coffers");
    bool capped=true,finite=true;for(const auto &post:p.pillars){double signedArea=0;for(size_t i=0;i<post.footprint.size();++i){auto a=post.footprint[i],b=post.footprint[(i+1)%post.footprint.size()];signedArea+=a.x*b.z-b.x*a.z;}capped&=post.capped&&signedArea<0;for(auto q:post.footprint)finite&=std::isfinite(q.x+q.y+q.z)&&post.top>post.base;}
    check(ok&&capped&&finite,"Roman columns have finite solid components and closed end caps");
    check(p.architecturalAgeYears==400&&std::abs(p.weathering-.68)<1e-8,"weathering derives from historical construction age");
    CellHouse again;DwellingPlan q;Ledger other;
    check(planCellDwelling(brief,settings,again,other,&error)&&realiseCellDwelling(again,settings,q,&error)&&q.rooms.size()==p.rooms.size()&&q.colonnadeColumns==p.colonnadeColumns&&q.cofferBeams==p.cofferBeams,"architectural planning is deterministic");
    brief.tags={"roman-temple"};brief.frontage={0};brief.setbackMetres=3.3;brief.frontageSetbackMetres=5.2;
    CellHouse temple;DwellingPlan templePlan;Ledger templeLedger;
    const bool templeOK=planCellDwelling(brief,settings,temple,templeLedger,&error)&&realiseCellDwelling(temple,settings,templePlan,&error);
    bool contained=templeOK;for(const auto &post:templePlan.pillars)for(auto q:post.footprint)contained &= q.x>=0&&q.x<=32&&q.z>=0&&q.z<=32;
    check(templeOK&&temple.courts.empty()&&templePlan.colonnadeColumns>=4&&templePlan.storeys.front().top>4.,"Roman temple reserves a tall cella and exterior four-column portico");
    check(templePlan.entranceLanding.size()==4 && templePlan.entranceFlight.size()==4,
          "portico blueprint reserves a supporting landing and separate wide stair flight");
    bool supported=templePlan.entranceLanding.size()==4;
    if(supported){const auto &r=templePlan.entranceLanding;double x0=1e30,z0=1e30,x1=-1e30,z1=-1e30;for(auto q:r){x0=std::min(x0,q.x);x1=std::max(x1,q.x);z0=std::min(z0,q.z);z1=std::max(z1,q.z);}
      for(const auto &column:templePlan.pillars)if(std::abs(column.base)<1e-6)for(auto q:column.footprint)supported &= q.x>=x0-1e-6 && q.x<=x1+1e-6 && q.z>=z0-1e-6 && q.z<=z1+1e-6;
    }
    check(supported,"every exterior column plinth rests inside its planned landing");
    bool paired=false;for(const auto &door:templePlan.doors)paired |= door.exterior && door.privateAccess && door.leaves==2;
    check(paired,"temple entrance retains paired door leaves in the dwelling blueprint");
    const auto templeStructure=hierarchy::buildSkeleton(templePlan);
    unsigned leftLeaves=0,rightLeaves=0;for(const auto &asset:templeStructure.structure.instances){leftLeaves+=asset.asset==hierarchy::FacadeAsset::DoorLeafLeft;rightLeaves+=asset.asset==hierarchy::FacadeAsset::DoorLeafRight;}
    check(leftLeaves>0 && leftLeaves==rightLeaves,"temple paired leaves survive into the actual facade instances");
    check(contained,"exterior columns and architrave remain inside the reserved building lot");
    CellDwellingSettings small;small.grid.maximumCells=20;small.grid.cellMetres=1.;tuneDwellingStyle(brief,small);
    check(small.courts==0,"compact plots preserve room space instead of forcing a courtyard");
  }
  if(argc>1 && std::string(argv[1])=="--dwelling-style")return failed?1:0;
  {
    generator::FortressJunctionBrief brief;brief.towerSides=12;brief.towerRadius=4.5;
    brief.roofHeight=13.;brief.sweptSpire=true;
    generator::FortressJunctionBlueprint roof;std::string error;
    check(generator::planFortressJunction(brief,roof,&error),"swept fantasy spire plans");
    std::map<std::pair<std::array<long long,3>,std::array<long long,3>>,unsigned> edges;
    const auto key=[](Position p){return std::array<long long,3>{std::llround(p.x*100000),std::llround(p.y*100000),std::llround(p.z*100000)};};
    bool finite=true;for(const auto &face:roof.roofFaces)for(size_t k=0;k<face.size();++k) {
      const auto p=face[k];finite &= std::isfinite(p.x+p.y+p.z) && p.y>=roof.belfryTop-1e-8 && p.y<=roof.roofApex+1e-8;
      auto a=key(p),b=key(face[(k+1)%face.size()]);if(b<a)std::swap(a,b);++edges[{a,b}];
    }
    bool closed=true;for(const auto &[edge,count]:edges)closed &= count==2;
    check(finite && closed && roof.roofFaces.size()==73,"multi-stage spire has shared rings, bounded height and a closed soffit");
  }
  {
    DwellingPlan p;
    DwellingWall wall;wall.exterior=true;wall.kind=WallKind::Solid;wall.a={0,0,0};wall.b={10,0,0};wall.height=4.2;p.walls.push_back(wall);
    p.storeys.emplace_back();p.storeys.back().top=30.;
    check(wallButtressHeight(p,{5,0,0},1,0)==0,"low wing omits buttress despite a tall main keep");
    p.walls.front().height=8.4;
    check(std::abs(wallButtressHeight(p,{5,0,0},1,0)-6.72)<1e-6,"support height follows the local wall stack");
    p.roofs.push_back({{{0,5,-2},{10,5,-2},{10,5,1},{0,5,1}},0,false,false});
    check(wallButtressHeight(p,{5,0,0},1,0)<=4.75,"lower roof caps the entire projecting support footprint");
    p.walls.front().height=3.;wall.a.y=wall.b.y=6.;wall.height=12.;p.walls.push_back(wall);
    check(wallButtressHeight(p,{5,0,0},1,0)==0,"a gap in the local wall stack cannot grow a tall support");
    check(gallerySeparation({0,0,0},{10,0,0},{5,0,-5},{5,0,5})==0,"crossing gallery envelopes are detected");
    check(gallerySeparation({0,0,0},{10,0,0},{0,0,2},{10,0,2})<2.9,"parallel galleries reserve the whole canopy width");
  }
  {
    generator::CompoundBrief manor;manor.id=6;manor.kind=generator::CompoundKind::Manor;
    manor.precinct={{0,0,0},{100,0,0},{100,0,90},{0,0,90}};manor.approach={50,0,-10};
    generator::CompoundPlan estate;generator::Ledger ledger;std::string error;
    bool compact=generator::planCompound(manor,[](double,double){return 0.;},estate,ledger,&error);
    for(const auto &wing:estate.wings)for(auto a:wing.brief.shell)for(auto b:wing.brief.shell)
      compact &= std::hypot(a.x-b.x,a.z-b.z)<40;
    check(compact && estate.wings.size()>=2,"large manor estate retains compact hall and service wings");
  }
  generator::CompoundBrief brief;brief.id=700;brief.seed=41;brief.kind=generator::CompoundKind::Castle;
  brief.fantasyGothic=true;brief.precinct={{0,0,0},{140,0,0},{140,0,120},{0,0,120}};brief.approach={70,0,-10};
  generator::CompoundPlan plan;generator::Ledger ledger;std::string error;
  check(generator::planCompound(brief,[](double,double){return 0.;},plan,ledger,&error),"large Gothic compound plans");
  check(plan.fantasyGothic && plan.towers.size()>=12 && plan.wings.size()>=5,"castle has tower hierarchy and distinct inhabited ranges");
  check(plan.dungeonRooms.size()==27,"three dungeon levels contain nine chambers each");
  bool contained=true;for(const auto &tower:plan.towers)for(auto p:tower.footprint)contained&=p.x>=0 && p.x<=140 && p.z>=0 && p.z<=120;
  check(contained,"towers remain within the reserved precinct");
  std::set<uint32_t> reached{0};bool changed=true;
  while(changed){changed=false;for(const auto &edge:plan.dungeonConnections)if(reached.count(edge.from) || reached.count(edge.to)){
    changed|=reached.insert(edge.from).second;changed|=reached.insert(edge.to).second;}}
  check(reached.size()==28,"every dungeon chamber is reachable from the keep entrance graph");
  bool doors=true;
  for(const auto &edge:plan.dungeonConnections)if(!edge.stairs) {
    unsigned on=0;
    for(const auto &room:plan.dungeonRooms)if(room.id==edge.from || room.id==edge.to) {
      for(size_t i=0;i<room.footprint.size();++i) {
        auto a=room.footprint[i],b=room.footprint[(i+1)%room.footprint.size()];
        const double dx=b.x-a.x,dz=b.z-a.z;
        const double t=((edge.position.x-a.x)*dx+(edge.position.z-a.z)*dz)/(dx*dx+dz*dz);
        if(t>0 && t<1 && std::abs((edge.position.x-a.x)*dz-(edge.position.z-a.z)*dx)<1e-6){++on;break;}
      }
    }
    doors&=on==2;
  }
  check(doors,"door connections lie on the shared wall of both chambers");
  for(const auto approach:std::vector<Position>{{70,0,-10},{150,0,60},{70,0,130},{-10,0,60}}) {
    auto rotated=brief;rotated.approach=approach;generator::CompoundPlan oriented;
    bool disjoint=generator::planCompound(rotated,[](double,double){return 0.;},oriented,ledger,&error);
    const auto bounds=[](const auto &ring){std::array<double,4> b{1e9,1e9,-1e9,-1e9};for(auto p:ring){b[0]=std::min(b[0],p.x);b[1]=std::min(b[1],p.z);b[2]=std::max(b[2],p.x);b[3]=std::max(b[3],p.z);}return b;};
    for(size_t i=0;i<oriented.wings.size();++i)for(size_t j=i+1;j<oriented.wings.size();++j) {
      const auto a=bounds(oriented.wings[i].brief.shell),b=bounds(oriented.wings[j].brief.shell);
      disjoint&=std::min(a[2],b[2])-std::max(a[0],b[0])<=.001 || std::min(a[3],b[3])-std::max(a[1],b[1])<=.001;
    }
    check(disjoint,"inhabited wings remain disjoint for every gate orientation");
  }
  for(uint32_t level=0;level<3;++level) {
    CityModelPlan model;
    CityModelSettings voxelSettings;size_t uniqueRooms=0,uniqueTriangles=0;
    voxelSettings.voxelUniqueMeshSink=[&](const CityVoxelUniqueMeshPart &part,const CityModelPlan &source,std::string *) {
      if(part.role!="dungeon-room" || part.triangleCount==0 ||
         part.firstTriangle+part.triangleCount>source.mesh.triangles.size())return false;
      ++uniqueRooms;uniqueTriangles+=part.triangleCount;return true;
    };
    check(buildCastleDungeonCutaway(plan,level,model,&error,&voxelSettings) && !model.mesh.triangles.empty(),"dungeon level emits native cutaway geometry");
    check(uniqueRooms==model.buildings.size() && uniqueTriangles==model.mesh.triangles.size(),
          "dungeon room assets cover the cutaway mesh");
    bool finite=true;for(auto p:model.mesh.vertices)finite&=std::isfinite(p.x+p.y+p.z);
    check(finite && model.mesh.triangles.size()==model.surfaceMaterials.size(),"dungeon mesh has finite vertices and complete material attributes");
  }
  check(plan.enclosure.size()==8,"curtain uses an angled eight-sided circuit");
  for(const auto layout:{generator::LandmarkLayout::GardenManor,generator::LandmarkLayout::Palace}) {
    auto estate=brief;estate.landmarkLayout=layout;generator::CompoundPlan p;
    check(generator::planCompound(estate,[](double,double){return 0.;},p,ledger,&error) && p.gardens.size()>=8 && p.fountains.size()==2 && p.towers.empty(),"estate profile reserves formal gardens and distinct inhabited wings");
  }
  {
    auto estate=brief;estate.kind=generator::CompoundKind::Manor;
    estate.landmarkLayout=generator::LandmarkLayout::GardenManor;
    estate.precinct={{0,0,0},{96,0,0},{96,0,86},{0,0,86}};
    estate.approach={48,0,-5};generator::CompoundPlan p;
    check(generator::planCompound(estate,[](double,double){return 0.;},p,ledger,&error) &&
          p.fantasyGothic && p.wings.size()==3 && p.plannedWings==3 && p.gardens.size()>=8,
          "world-sized manor keeps its Gothic estate layout below the citadel footprint minimum");
  }
  {
    auto estate=brief;estate.landmarkLayout=generator::LandmarkLayout::GardenManor;
    generator::CompoundPlan p;generator::planCompound(estate,[](double,double){return 0.;},p,ledger,&error);
    SettlementFabric fabric;fabric.terraceLevel=0;fabric.compounds={p};
    CityCompletionPlan city;city.revision=1;CityModelSettings settings;settings.groundVertexSpacing=4.;CityModelPlan model;
    const bool built=buildCityModel(city,fabric,settings,model,&error);
    if(!built)std::cout<<error<<"\n";
    check(built && model.buildingLinks.size()==2,"native garden manor connects all three realised wings through two galleries");
    bool distinct=true;for(const auto &link:model.buildingLinks)distinct&=link.from!=link.to && link.width>=1.6 && std::hypot(link.a.x-link.b.x,link.a.z-link.b.z)>.2;
    check(distinct,"wing connections export distinct endpoints and clear passage widths");
    check(model.mesh.triangles.size()==model.triangleUVs.size() && model.mesh.triangles.size()==model.surfaceMaterials.size(),"complete estate mesh retains one UV and material record per triangle");
  }
  {
    auto attached=brief;attached.keepPlacement=generator::KeepPlacement::WallAttached;
    attached.seed=5353516529563775243ULL;
    attached.precinct={{1200,0,300},{1380,0,300},{1380,0,440},{1200,0,440}};
    attached.approach={1290,0,290};
    attached.extraStoreys=1;generator::CompoundPlan p;
    const bool planned=generator::planCompound(attached,[](double,double){return 0.;},p,ledger,&error);
    check(planned && p.wallAttachedKeep && p.yards.size()>=2 && p.wallAccess.size()==4,"attached keep reserves garden yards and four upper wall connections");
    bool levels=true;for(const auto &a:p.wallAccess)levels&=std::abs(a.at.y/4.2-std::round(a.at.y/4.2))<1e-6;
    check(levels,"wall entries align with occupied storey levels");
    double yardArea=0;for(const auto &yard:p.yards) {double a=0;for(size_t i=0;i<yard.footprint.size();++i){auto u=yard.footprint[i],v=yard.footprint[(i+1)%yard.footprint.size()];a+=u.x*v.z-v.x*u.z;}yardArea+=std::abs(a)*.5;}
    check(yardArea>2000,"attached keep leaves substantial usable yard area");
    SettlementFabric fabric;fabric.terraceLevel=0;fabric.compounds={p};
    CityCompletionPlan city;city.revision=1;CityModelSettings settings;settings.groundVertexSpacing=4.;CityModelPlan model;
    const bool built=buildCityModel(city,fabric,settings,model,&error);if(!built)std::cout<<error<<'\n';
    check(built && model.wallAccessLinks.size()==p.wallAccess.size(),"every requested elevated entry produces a realised doorway and bridge");
    bool thresholds=true;for(const auto &link:model.wallAccessLinks)thresholds&=link.floor>=3 && link.clearWidth>=1.6 && std::abs(link.door.y-link.wall.y)<=.051;
    check(thresholds,"elevated door thresholds meet the wall walks within finished-floor allowance");
    attached.extraStoreys=5;check(!generator::planCompound(attached,[](double,double){return 0.;},p,ledger,&error),"unbounded additional storeys are rejected");
  }
  {
    auto world=brief;world.id=7;world.seed=5353516529563775243ULL;
    world.foundation=.702100853107;world.approach={1305.5131868869169,0,440.18937768142314};
    generator::CompoundPlan p;
    const bool fitted=generator::fitCompoundPrecinct(historicalCastlePrecinct(),5.45,world.precinct,&error);
    const bool planned=fitted && generator::planCompound(world,[](double,double){return 0.;},p,ledger,&error);
    SettlementFabric fabric;fabric.terrain=[](double,double){return 0.;};fabric.compounds={p};
    CityCompletionPlan city;city.revision=1;CityModelSettings settings;CityModelPlan model;
    const bool built=planned && buildCityModel(city,fabric,settings,model,&error);
    if(!built)std::cout<<error<<'\n';
    check(built && model.wallAccessLinks.size()==p.wallAccess.size(),"actual world-cell castle resolves every elevated entrance");
    bool bent=false,continuous=true;
    for(const auto &link:model.wallAccessLinks) {
      bent|=link.path.size()==3;
      continuous&=link.path.size()>=2 && std::hypot(link.path.front().x-link.wall.x,link.path.front().z-link.wall.z)<1e-6 &&
                  std::hypot(link.path.back().x-link.door.x,link.path.back().z-link.door.z)<1e-6;
      for(auto point:link.path)continuous&=std::isfinite(point.x+point.y+point.z) && std::abs(point.y-link.door.y)<=.051;
    }
    check(bent && continuous,"off-axis wall entrance retains its landing, endpoints and continuous floor height");
  }
  {
    generator::CellDwellingBrief wing;wing.id=912;wing.seed=41;wing.building=generator::BuildingClass::Castle;wing.floors=4;
    wing.lot.outer={{0,0},{procedural::geom2::toUnit(30.),0},{procedural::geom2::toUnit(30.),procedural::geom2::toUnit(24.)},{0,procedural::geom2::toUnit(24.)}};
    wing.capacity=generator::CellDwellingBrief::Capacity{720.,24,30.,true};
    generator::CellDwellingSettings settings;settings.capacityCellMetres=2.4;settings.doorWidthMetres=1.6;settings.minimumCorridorMetres=2.4+settings.innerWallMetres;settings.outerWallMetres=.8;
    generator::CellHouse house;DwellingPlan dwelling;
    const bool ok=generator::planCellDwelling(wing,settings,house,ledger,&error) && generator::realiseCellDwelling(house,settings,dwelling,&error);
    if(!ok)std::cout<<error<<'\n';
    bool generous=ok && !dwelling.doors.empty();for(const auto &door:dwelling.doors)generous&=door.width>=1.5;
    check(generous,"castle capacity planning preserves generous internal door and arch widths");
    check(ok && std::abs(house.frame.cellMetres-2.4)<1e-6,"castle capacity does not silently replace the circulation grid with metre cells");
  }
  generator::FortressJunctionBrief tower;tower.towerSides=12;tower.towerRadius=5.5;
  tower.height=25.2;tower.floors=6;tower.inhabitedFloors=true;tower.inward={1,0,1};
  tower.elevatedDoors={{1,12.6,0},{-1,16.8,0}};
  generator::FortressJunctionBlueprint bp;
  check(generator::planFortressJunction(tower,bp,&error),"inhabited tower builds wall-walk thresholds");
  for(double radius:{4.2,4.5,5.5}) {
    tower.towerRadius=radius;generator::FortressJunctionBlueprint varied;
    check(generator::planFortressJunction(tower,varied,&error) && generator::access::validate(varied.towerAccess).empty(),"small and large inhabited towers retain safe stair landings");
  }
  bool outward=true;for(const auto &roof:bp.roofFaces){const auto a=roof[0],b=roof[1],c=roof[2];outward&=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z)>0;}
  check(outward,"tower roof triangles face upward for back-face culling");
  auto faults=generator::access::validate(bp.towerAccess);
  for(const auto &fault:faults)std::cout<<"ACCESS "<<fault.what<<"\n";
  check(faults.empty(),"tower floors and return stairs pass geometric access validation");
  const auto reachedFloors=generator::access::reachable(bp.towerAccess);
  check(std::count(reachedFloors.begin(),reachedFloors.end(),true)==bp.towerAccess.surfaces.size(),"every tower floor is reachable from ground");
  check(bp.openings.size()==3 && bp.towerAccess.portals.size()==2,"ground entrance and both elevated passages are explicit openings");
  {
    auto rotated=tower;rotated.towerRotation=.37;
    generator::FortressJunctionBlueprint oriented;
    check(generator::planFortressJunction(rotated,oriented,&error),"rotated curtain tower plans");
    check(std::abs(oriented.tower.front().x-rotated.towerRadius*std::sin(.37))<1e-8,
      "tower shell preserves the curtain clipping footprint orientation");
    bool landingHole=true;
    for(const auto &surface:bp.towerAccess.surfaces)if(surface.kind==generator::access::SurfaceKind::Storey) {
      double end=-1e9;for(const auto &hole:surface.holes)for(auto p:hole)end=std::max(end,p.z);
      landingHole &= end>=2.6;
    }
    check(landingHole,"stairwell includes return landing for headroom on every storey");
    auto structure=generator::buildFortressJunctionStructure(bp);
    bool top=false,underside=false,sill=false;
    for(const auto &piece:structure.pieces)for(auto id:piece.faces) {
      const auto &f=structure.faces[id];bool flat=true;for(auto p:f)flat &= std::abs(p.y-f[0].y)<1e-7;
      if(!flat)continue;
      if(piece.kind==generator::FortressPieceKind::FloorSlab) {
        top |= std::abs(f[0].y-tower.height)<1e-7;
        underside |= std::abs(f[0].y-tower.height+.18)<1e-7;
      }
      if(piece.kind==generator::FortressPieceKind::Tower)sill |= std::abs(f[0].y-12.6)<1e-7;
    }
    check(top && underside,"lookout has a solid floor slab with ceiling underneath");
    check(sill,"wall-walk door has a sill spanning tower wall thickness");
    rotated.elevatedDoors={{1,13.,0}};
    check(!generator::planFortressJunction(rotated,oriented,&error),"reject floating wall doorway between tower storeys");
  }
  {
    // The wall walk is 2.8 m wide on the curtain's axis. A door on the
    // middle of the face nearest that axis landed half inside the parapet
    // whenever the axis met the tower on a corner, which a twelve-sided
    // tower turned half a face does for every bevelled curtain.
    auto bevel=tower;bevel.towerRotation=3.14159265358979/12;
    const double diagonal=std::sqrt(.5);
    bevel.elevatedDoors={{1,12.6,0},{diagonal,16.8,diagonal}};
    generator::FortressJunctionBlueprint turned;
    check(generator::planFortressJunction(bevel,turned,&error),"tower with straight and bevelled curtains plans");
    bool onWalk=true;uint32_t elevated=0;
    for(const auto &opening:turned.openings)if(opening.bottom>bevel.base+.1) {
      ++elevated;
      const auto &direction=std::abs(opening.bottom-12.6)<1e-6?bevel.elevatedDoors[0]:bevel.elevatedDoors[1];
      const double ux=direction.x/std::hypot(direction.x,direction.z),uz=direction.z/std::hypot(direction.x,direction.z);
      const auto a=turned.towerInner[opening.face],b=turned.towerInner[(opening.face+1)%turned.towerInner.size()];
      const double span=std::hypot(b.x-a.x,b.z-a.z);
      for(const double t:{opening.along-opening.width*.5/span,opening.along+opening.width*.5/span}) {
        const Position p{a.x+(b.x-a.x)*t,0,a.z+(b.z-a.z)*t};
        onWalk&=std::abs(ux*(p.z-bevel.centre.z)-uz*(p.x-bevel.centre.x))<=1.4+1e-6;
      }
    }
    check(elevated==2 && onWalk,"every wall-walk door opens within the promenade, on a straight curtain and on a bevel");
  }
  {
    // A return stair is solid: each flight has its treads and risers, a
    // stringer down each side and a soffit; a spine wall closes the slot
    // between the two flights from the ground to above the lookout; and the
    // open edge of every flight carries a rail that climbs with it.
    const auto structure=generator::buildFortressJunctionStructure(bp);
    uint32_t flights=0;bool solid=true;
    for(const auto &piece:structure.pieces)if(piece.kind==generator::FortressPieceKind::StairFlight) {
      ++flights;uint32_t stringers=0,undersides=0;
      for(auto id:piece.faces) {
        const auto &f=structure.faces[id];
        if(f.size()>=8)++stringers;
        double low=1e9,high=-1e9;bool flat=true;for(auto p:f){low=std::min(low,p.y);high=std::max(high,p.y);flat&=std::abs(p.y-f[0].y)<1e-7;}
        if(f.size()==4 && !flat && high-low>.5)++undersides;
      }
      solid&=stringers==2 && undersides>=1;
    }
    check(flights==bp.towerAccess.ramps.size() && solid,"every flight is a solid with two stringers and a soffit");
    bool spine=false;
    for(const auto &piece:structure.pieces)if(piece.kind==generator::FortressPieceKind::Wall)
      for(auto id:piece.faces) {
        double low=1e9,high=-1e9;for(auto p:structure.faces[id]){low=std::min(low,p.y);high=std::max(high,p.y);}
        spine|=low<=bp.base+1e-6 && high>=bp.base+bp.height+.9;
      }
    check(spine,"a spine wall closes the slot between the flights from the ground to above the lookout");
    uint32_t sloped=0;for(const auto &rail:bp.towerAccess.barriers)sloped+=!rail.wall && std::abs(rail.a.y-rail.b.y)>.5;
    check(sloped==bp.towerAccess.ramps.size(),"every flight has a rail along its open edge");
    check(generator::access::validate(bp.towerAccess).empty(),"the fenced stair still passes access validation");
  }
  {
    // A city wall tower: ten sides, a spiral stair round the inside, a door
    // from the ground and one onto the wall walk in each direction. The walk
    // door must open onto a turn landing - level ground - and the spiral's
    // well side must be railed all the way up.
    generator::FortressJunctionBrief small;small.towerSides=10;small.towerRadius=2.9;small.towerWallDepth=.35;small.stairWidth=.8;
    small.height=14.4;small.floors=4;small.inward={0,0,1};small.elevatedDoors={{1,7.2,0},{-1,7.2,0}};
    generator::FortressJunctionBlueprint spiral;
    const bool planned=generator::planFortressJunction(small,spiral,&error);
    if(!planned)std::cout<<error<<'\n';
    check(planned,"a ten-sided wall tower with walk doors plans");
    uint32_t walkDoors=0;bool onLandings=true,nearWalk=true;
    for(const auto &opening:spiral.openings)if(opening.bottom>small.base+.1) {
      ++walkDoors;bool landing=false;
      for(const auto &surface:spiral.towerAccess.surfaces)
        landing|=surface.kind==generator::access::SurfaceKind::Landing && std::abs(surface.height-opening.bottom)<1e-6;
      onLandings&=landing;nearWalk&=opening.bottom>=7.2-.05 && opening.bottom<=7.2+10.8/3.*.5+.05;
    }
    check(walkDoors==2 && onLandings,"each walk door opens onto a turn landing");
    check(nearWalk,"a walk door's sill is at the walk or within half a storey above it");
    const auto spiralFaults=generator::access::validate(spiral.towerAccess);
    for(const auto &fault:spiralFaults)std::cout<<"ACCESS "<<fault.what<<"\n";
    check(spiralFaults.empty(),"the spiral stair, its rails and its doors pass access validation");
    uint32_t flightRails=0,turnRails=0;
    for(const auto &rail:spiral.towerAccess.barriers){flightRails+=rail.note=="flight rail";turnRails+=rail.note=="turn rail";}
    check(flightRails==spiral.towerAccess.ramps.size() && turnRails>0,"every flight and every turn is railed on the well side");
    const auto built=generator::buildFortressJunctionStructure(spiral);
    bool solidFlights=true,solidTurns=true;uint32_t flights=0,turns=0;
    for(const auto &piece:built.pieces) {
      if(piece.kind==generator::FortressPieceKind::StairFlight){++flights;uint32_t stringers=0;for(auto id:piece.faces)stringers+=built.faces[id].size()>=8;solidFlights&=stringers==2;}
      if(piece.kind==generator::FortressPieceKind::StairTurn){++turns;solidTurns&=piece.faces.size()>=6;}
    }
    check(flights==spiral.towerAccess.ramps.size() && solidFlights,"every spiral flight is a solid with two stringers");
    check(turns>0 && solidTurns,"every turn landing is a slab with an underside and a skirt");
    // A tower too short to carry the far door's landing under its lookout
    // keeps the near door and drops the far one, rather than cutting a door
    // through the deck.
    auto shortTower=small;shortTower.height=10.8;shortTower.floors=3;
    generator::FortressJunctionBlueprint stub;
    uint32_t served=0;
    const bool shortPlanned=generator::planFortressJunction(shortTower,stub,&error);
    for(const auto &opening:stub.openings)served+=opening.bottom>shortTower.base+.1;
    check(shortPlanned && served==1 && generator::access::validate(stub.towerAccess).empty(),"a short tower serves the door it can and stays valid");
  }
  {
    const auto inside=[](const std::vector<Position> &ring,Position p) {
      bool in=false;for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
        const auto a=ring[i],b=ring[j];
        if((a.z>p.z)!=(b.z>p.z) && p.x<(b.x-a.x)*(p.z-a.z)/(b.z-a.z)+a.x)in=!in;
      }return in;
    };
    for(unsigned bands:{3u,4u})for(unsigned side=0;side<4;++side) {
      generator::CompoundBrief terraced;terraced.id=900;terraced.seed=41;terraced.fantasyGothic=true;
      terraced.kind=generator::CompoundKind::Castle;terraced.dungeonLevels=0;terraced.terraceBands=bands;
      terraced.precinct={{0,0,0},{180,0,0},{180,0,180},{0,0,180}};
      terraced.approach=side==0?Position{90,0,-10}:side==1?Position{190,0,90}:side==2?Position{90,0,190}:Position{-10,0,90};
      generator::CompoundPlan stepped;
      const bool ok=generator::planCompound(terraced,[](double,double){return 0.;},stepped,ledger,&error);
      check(ok && stepped.terraces.size()==bands && stepped.terraceLinks.size()==bands-1,"terrace bands connect for every entrance orientation");
      bool supported=ok;
      for(const auto &w:stepped.wings) {
        bool onDeck=false;
        for(const auto &t:stepped.terraces)if(std::abs(w.brief.foundation-t.level)<1e-6) {
          bool fits=true;for(auto p:w.brief.shell)fits &= inside(t.footprint,p);onDeck |= fits;
        }
        supported &= onDeck;
      }
      check(supported,"every terraced building footprint fits wholly on its assigned deck");
      bool routes=ok;
      for(const auto &link:stepped.terraceLinks) {
        routes &= link.to==link.from+1 && link.width>=4. && (link.head.y-link.foot.y)/link.steps<=.18;
        for(unsigned n=1;n<=link.steps;++n) {
          Position p{link.foot.x+(link.head.x-link.foot.x)*n/link.steps,0,link.foot.z+(link.head.z-link.foot.z)*n/link.steps};
          for(const auto &w:stepped.wings)routes &= !inside(w.brief.shell,p);
        }
      }
      check(routes,"climbing route reserves broad clear stairs with safe risers");
      if(ok && bands==3 && side==0) {
        SettlementFabric fabric;fabric.terraceLevel=0;fabric.compounds={stepped};
        CityCompletionPlan city;city.revision=1;CityModelSettings settings;settings.groundVertexSpacing=4.;CityModelPlan model;
        const bool meshed=buildCityModel(city,fabric,settings,model,&error);
        check(meshed,"terraced castle produces a complete native model");
        size_t rock=0;for(auto material:model.surfaceMaterials)rock+=material==27;
        check(rock>0 && rock<20000,"terraced castle exposes a bounded faceted bedrock foundation");
        bool clear=meshed;
        for(const auto &link:stepped.terraceLinks)for(unsigned step=0;step<link.steps;++step) {
          const double t=(step+.5)/link.steps;
          const Position probe{link.foot.x+(link.head.x-link.foot.x)*t,0,link.foot.z+(link.head.z-link.foot.z)*t};
          double highest=-1e9;
          for(auto tri:model.mesh.triangles) {
            const auto a=model.mesh.vertices[tri[0]],b=model.mesh.vertices[tri[1]],c=model.mesh.vertices[tri[2]];
            if(std::abs(a.y-b.y)>1e-6 || std::abs(a.y-c.y)>1e-6)continue;
            const auto cross=[](auto a,auto b,auto p){return (b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x);};
            const double u=cross(a,b,probe),v=cross(b,c,probe),w=cross(c,a,probe);
            if(std::abs(cross(a,b,c))>1e-8 && ((u>=-1e-6 && v>=-1e-6 && w>=-1e-6)||(u<=1e-6 && v<=1e-6 && w<=1e-6)))highest=std::max(highest,a.y);
          }
          const double expected=link.foot.y+(link.head.y-link.foot.y)*(step+1)/link.steps+.02;
          if(std::abs(highest-expected)>=1e-5)std::cout<<"STAIR "<<link.to<<" step "<<step<<" actual "<<highest<<" expected "<<expected<<" at "<<probe.x<<","<<probe.z<<"\n";
          clear &= std::abs(highest-expected)<1e-5;
        }
        check(clear,"actual terrace mesh preserves every stair tread without a cap bridging the notch");
      }

    }
  }
  brief.precinct={{0,0,0},{40,0,0},{40,0,40},{0,0,40}};
  check(!generator::planCompound(brief,[](double,double){return 0.;},plan,ledger,&error),"large-castle profile refuses undersized land");
  return failed?1:0;
}
