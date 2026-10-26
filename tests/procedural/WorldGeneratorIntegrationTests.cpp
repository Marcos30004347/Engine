#include "procedural/world/generator/CurtainWall.hpp"
#include "fixtures/EstateCastlePrecinct.hpp"
#include "procedural/world/WorldDomainGeneratorAdapters.hpp"
#include "procedural/world/WorldGenerationEdits.hpp"
#include "procedural/world/WorldNativeGenerator.hpp"
#include "procedural/world/WorldPatchGeneratorAdapters.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/WorldSettlementFabricAdapter.hpp"
#include "procedural/world/WorldSettlementSiteAdapter.hpp"
#include "procedural/world/generator/BlockGrammar.hpp"
#include "procedural/world/generator/CompoundPlanner.hpp"
#include "procedural/world/generator/SettlementGenerators.hpp"
#include "procedural/geom3/Prism.hpp"
#include "procedural/geom2/Boolean.hpp"
#include "procedural/world/generator/CellDwellingSite.hpp"
#include "procedural/world/generator/CellDwellingRoof.hpp"
#include "procedural/world/generator/CellDwellingRealise.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace procedural::world::rewrite;
int main(int argc, char **argv)
{
  int failed = 0, checks = 0;
  std::string error;
  auto check = [&](bool ok, const char *name)
  {
    ++checks;
    failed += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << name << " " << error << '\n';
    error.clear();
  };

  {
    namespace g=procedural::geom2;
    CityCompletionPlan city;city.revision=1;
    const std::vector<Position> ring={{0,0,0},{24,0,0},{24,0,24},{0,0,24}};
    city.skeleton.regions.push_back({10,10,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Residential,ring,false,576});
    CityLotDevelopment lot;lot.lot=10;lot.developed=true;lot.desiredRooms=4;lot.preferredFootprintMetres2=200;lot.minimumFloors=lot.maximumFloors=1;city.lots.push_back(lot);
    CitySmoothedRoute route;route.id=1;route.path={{-4,0,-4},{28,0,-4}};route.halfWidth=2.;city.routes.push_back(route);
    SettlementFabric fabric;fabric.terraceLevel=0;CityModelSettings settings;settings.raisedLotAprons=true;
    CityModelPlan model;bool built=buildCityModel(city,fabric,settings,model,&error);
    check(built&&model.buildings.size()==1,"raised lot fixture emits its dwelling and stone cap");
    g::Polygon lotPolygon;for(auto q:ring)lotPolygon.outer.push_back({g::toUnit(q.x),g::toUnit(q.z)});
    double overlap=0;bool valid=built;
    for(const auto &piece:model.surfaces.pieces){std::vector<g::Polygon> common;
      valid &= g::intersectionOf({g::toOperand(lotPolygon),g::toOperand(piece.polygon)},common,&error);
      for(const auto &p:common)overlap+=std::abs(g::areaMetres(p));}
    check(valid&&overlap<1e-5,"successful lot cap removes the underlying terrain ownership instead of stacking stone skins");
    check(model.mesh.triangles.size()==model.materials.size()&&model.mesh.triangles.size()==model.surfaceMaterials.size()&&model.mesh.triangles.size()==model.groundWeights.size(),"deferred ground emission preserves all mesh attribute arrays");
    settings.instanceDwellingPanels=true;
    CityModelPlan compact;const bool compactBuilt=buildCityModel(city,fabric,settings,compact,&error);
    check(compactBuilt && !compact.quadInstances.empty() && compact.mesh.triangles.size()+2*compact.quadInstances.size()==model.mesh.triangles.size(),
          "instanced dwelling keeps complete triangle coverage without expanding its panels");
    const auto materialAreas=[](const CityModelPlan &m) {
      std::map<uint32_t,double> areas;
      const auto area=[](Position a,Position b,Position c){const double ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z,vx=c.x-a.x,vy=c.y-a.y,vz=c.z-a.z;return .5*std::hypot(uy*vz-uz*vy,uz*vx-ux*vz,ux*vy-uy*vx);};
      for(size_t i=0;i<m.mesh.triangles.size();++i){const auto t=m.mesh.triangles[i];const auto a=m.mesh.vertices[t[0]],b=m.mesh.vertices[t[1]],c=m.mesh.vertices[t[2]];areas[m.surfaceMaterials[i]]+=area({a.x,a.y,a.z},{b.x,b.y,b.z},{c.x,c.y,c.z});}
      for(const auto &q:m.quadInstances){const auto p=[&](size_t i){return Position{q[i*3],q[i*3+1],q[i*3+2]};};areas[uint32_t(q[21])]+=area(p(0),p(1),p(2))+area(p(0),p(2),p(3));}
      return areas;
    };
    const auto expected=materialAreas(model),actual=materialAreas(compact);bool equal=actual.size()==expected.size();
    for(const auto &[id,area]:expected){const auto it=actual.find(id);equal &= it!=actual.end() && std::abs(it->second-area)<.001;}
    check(equal,"instanced dwelling preserves surface area for every material");
    bool finite=true;for(const auto &q:compact.quadInstances)for(double v:q)finite &= std::isfinite(v);
    check(finite && compact.buildings.size()==model.buildings.size() && compact.buildings.front().rooms==model.buildings.front().rooms,
          "instance records retain finite geometry, UVs and building inspection metadata");
    settings.instanceDwellingPanels=false;
    city.skeleton.regions.front().parent=100;
    CityReliefPlan relief;CityReliefPlan::RaisedBlock deck;deck.block=100;deck.boundary=ring;deck.level=0;deck.foundation=-.3;relief.platforms.push_back(deck);
    CityModelPlan elevated;const bool elevatedBuilt=buildCityModel(city,fabric,settings,elevated,&error,&relief);
    check(elevatedBuilt&&elevated.buildings.size()==1,"terrain subtraction under structural decks preserves the semantic lot and its dwelling");
  }
  if(argc>1&&std::string(argv[1])=="--ground-ownership")return failed?1:0;
  {
    namespace g=procedural::geom2;
    CitySurfaceStructure surfaces;
    CitySurfacePiece left,right;
    left.polygon.outer={{0,0},{4000000,0},{4000000,8000000},{0,8000000}};
    right.polygon.outer={{4000000,0},{8000000,0},{8000000,8000000},{4000000,8000000},{4000000,3000000}};
    surfaces.pieces={left,right};
    nodeCitySurfaceBoundaries(surfaces);
    const auto &ring=surfaces.pieces.front().polygon.outer;
    check(ring.size()==5 && std::find(ring.begin(),ring.end(),g::Point{4000000,3000000})!=ring.end(),
      "ground ownership neighbours retain identical shared boundary nodes");
    double area=0;bool valid=true;
    for(const auto &piece:surfaces.pieces) {
      std::vector<g::Point> vertices;std::vector<std::array<uint32_t,3>> triangles;
      valid &= procedural::geom3::triangulate(piece.polygon,triangles,vertices,&error);
      area+=g::areaMetres(piece.polygon);
    }
    check(valid && std::abs(area-64)<1e-9,"ground boundary noding preserves triangulation and surface coverage");
    CityCompletionPlan completion;completion.revision=1;
    for(size_t i=0;i<surfaces.pieces.size();++i) {
      CitySkeletonRegion region;region.id=i+1;region.kind=CityRegionKind::Block;
      for(auto p:surfaces.pieces[i].polygon.outer)region.boundary.push_back({g::toMetres(p.x),0,g::toMetres(p.y)});
      completion.skeleton.regions.push_back(region);
    }
    SettlementFabric fabric;fabric.terrain=[](double x,double z){return .03*x*x+.02*z*z;};
    CityModelSettings settings;settings.groundOnly=true;settings.groundVertexSpacing=2;
    CityModelPlan model;bool closed=buildCityModel(completion,fabric,settings,model,&error);
    std::map<std::pair<uint32_t,uint32_t>,unsigned> edges;
    for(auto triangle:model.mesh.triangles)for(size_t k=0;k<3;++k){auto a=triangle[k],b=triangle[(k+1)%3];if(a>b)std::swap(a,b);++edges[{a,b}];}
    size_t cracks=0;
    for(auto [edge,count]:edges)if(count!=2) {
      auto a=model.mesh.vertices[edge.first],b=model.mesh.vertices[edge.second];
      const bool boundary=(std::abs(a.x+12)<1e-6 && std::abs(b.x+12)<1e-6) ||
        (std::abs(a.x-20)<1e-6 && std::abs(b.x-20)<1e-6) ||
        (std::abs(a.z+12)<1e-6 && std::abs(b.z+12)<1e-6) ||
        (std::abs(a.z-20)<1e-6 && std::abs(b.z-20)<1e-6);
      cracks+=!boundary;
    }
    check(closed && cracks==0,"nonlinear terrain has no unmatched interior ground edges");
  }
  {
    namespace gen=procedural::world::generator;
    const std::vector<gen::ConvexRing> cells={{{1311.3604396482813,0.0,1336.57339160035},{1328.840923609891,0.0,1356.9778153930513},{1290.3262224283276,0.0,1403.9833989863475},{1260.9331022341862,0.0,1384.0726900678208},{1285.0623350368846,0.0,1361.3445094923754}},{{1256.2368399456684,0.0,1340.935882508378},{1278.208272819701,0.0,1321.6928729971114},{1309.9006720158814,0.0,1327.0997741893702},{1311.3604396482813,0.0,1336.57339160035},{1285.0623350368846,0.0,1361.3445094923754}},{{1267.6723207173955,0.0,1294.2096174881403},{1278.208272819701,0.0,1321.6928729971114},{1256.2368399456684,0.0,1340.935882508378},{1249.289050314743,0.0,1337.1296355379789}}};
    gen::ConvexRing precinct;
    const bool narrow=gen::fitCompoundPrecinct(cells,5.8,precinct,&error);
    check(!narrow,"narrow world castle site remains rejected rather than crossing its street margin");
    const std::vector<gen::ConvexRing> adjoining{{{0,0,0},{24,0,0},{24,0,64},{0,0,64}},{{24,0,0},{48,0,0},{48,0,64},{24,0,64}}};
    const bool fitted=gen::fitCompoundPrecinct(adjoining,5.8,precinct,&error);
    check(fitted,"world castle precinct spans adjacent planning cells without reserving internal seams");
    gen::ConvexRing estate;
    const std::vector<gen::ConvexRing> strip{{{0,0,0},{400,0,0},{400,0,60},{0,0,60}}};
    check(!gen::fitCompoundPrecinct(strip,6.,estate,&error,90.,82.),
          "large but narrow estate cannot satisfy garden manor dimensions");
    const std::vector<gen::ConvexRing> estateCells{{{0,0,0},{60,0,0},{60,0,120},{0,0,120}},
                                                {{60,0,0},{120,0,0},{120,0,120},{60,0,120}}};
    check(gen::fitCompoundPrecinct(estateCells,6.,estate,&error,90.,82.),
          "adjacent estate cells reserve a usable garden manor beyond street margins");
    gen::CompoundBrief brief;brief.id=8;brief.seed=17;brief.kind=gen::CompoundKind::Castle;brief.precinct=precinct;
    gen::CompoundPlan compound;gen::Ledger ledger;
    const bool planned=fitted && gen::planCompound(brief,[](double,double){return 0.;},compound,ledger,&error);
    check(planned && compound.plannedWings>0,"the world-selected castle precinct realizes its keep and ranges");
  }
  {
    namespace gen=procedural::world::generator;
    gen::CompoundBrief brief;brief.id=7;brief.seed=41;brief.kind=gen::CompoundKind::Castle;brief.fantasyGothic=true;
    const bool fit=gen::fitCompoundPrecinct(estateCastlePrecinct(),5.8,brief.precinct,&error,122.,112.);
    gen::CompoundPlan castle;gen::Ledger ledger;
    check(fit && gen::planCompound(brief,[](double,double){return 0.;},castle,ledger,&error),
          "historical estate castle region realizes a Gothic keep without compact fallback");
    if(!error.empty())std::cerr<<error<<'\n';
  }
  {
    namespace gen=procedural::world::generator;
    gen::CurtainWallInput input;
    input.outline={{0,0,0},{120,0,0},{150,0,40},{120,0,120},{0,0,120}};
    input.cells={input.outline};input.pinnedRings={input.outline};input.sectionMetres=75;
    gen::CurtainWallPlan wall;gen::Ledger ledger;
    bool kept=gen::CurtainWall::plan(input,wall,ledger,&error);
    for(auto p:input.outline) kept &= std::any_of(wall.ring.begin(),wall.ring.end(),[&](auto q){return std::hypot(p.x-q.x,p.z-q.z)<1e-6;});
    check(kept,"wall resampling retains pinned estate corners instead of cutting across their land");
  }
  if(argc>1 && std::string(argv[1])=="--compound-precinct")return failed?1:0;
  CityPlanGenerationBrief city;
  {
    namespace gen=procedural::world::generator;namespace g=procedural::geom2;
    gen::CellDwellingBrief brief;brief.id=9493707473874490773ull;brief.seed=brief.id^0x9E3779B97F4A7C15ull;
    brief.building=gen::BuildingClass::ShopHouse;brief.standing=gen::PlotStanding::Modest;
    for(auto p:std::vector<Position>{{1406.4193575055033,0,1562.5805806143644},{1415.8449670872824,0,1569.9187564285414},{1412.1753785875305,0,1574.6322042117936},{1402.7497690057514,0,1567.2940283976166}})brief.lot.outer.push_back({g::toUnit(p.x),g::toUnit(p.z)});
    brief.frontage={0};brief.party={2};brief.capacity=gen::CellDwellingBrief::Capacity{71.35520416614963,3,15,true,true};brief.capacity->minimumSpanCells=4;brief.capacity->minimumFloors=2;
    gen::CellDwellingSettings settings;settings.maximumFloors=3;settings.eavesOverhangMetres=0;settings.footprintAttempts=24;
    gen::CellHouse house;gen::Ledger ledger;gen::CellRoofs roofs;gen::Stream stream(brief.seed);
    const bool planned=gen::planCellDwelling(brief,settings,house,ledger,&error);
    const bool roofed=planned && gen::planRoofs(house,settings,stream,roofs,ledger,&error);
    std::vector<g::Operand> operands;double sum=0;
    for(const auto &f:roofs.faces)if(!f.gable && !f.soffit){g::Polygon p;for(auto q:f.polygon)p.outer.push_back({g::toUnit(q.x),g::toUnit(q.z)});if(!g::counterClockwise(p.outer))g::reverse(p.outer);if(g::areaMetres(p)>1e-8){sum+=g::areaMetres(p);operands.push_back(g::toOperand(p));}}
    std::vector<g::Polygon> covered;const bool united=roofed && g::unionOf(operands,covered,&error);double area=0;for(const auto &p:covered)area+=g::areaMetres(p);
    g::Polygon footprint;if(planned)g::polygonOf(house.grid,house.frame,house.floors.front().area,footprint);
    std::cout<<"ROOF_PROBE floors="<<house.floors.size()<<" faces="<<roofs.faces.size()<<" covered="<<area<<" sum="<<sum<<" footprint="<<g::areaMetres(footprint)<<"\n";
    check(united && std::abs(area-g::areaMetres(footprint))<.001,"rotated shop-house roof covers its occupied footprint");
    check(std::any_of(roofs.faces.begin(),roofs.faces.end(),[](const auto &f){return f.gable && f.wing==1;}),"top-storey party roof retains its vertical gable closure without a taller neighbour");
    DwellingPlan realised;const bool realisedOK=planned && gen::realiseCellDwelling(house,settings,realised,&error);
    operands.clear();
    const auto addRoof=[&](const auto &ring){g::Polygon p;for(auto q:ring)p.outer.push_back({g::toUnit(q.x),g::toUnit(q.z)});if(!g::counterClockwise(p.outer))g::reverse(p.outer);if(g::areaMetres(p)>1e-8)operands.push_back(g::toOperand(p));};
    for(const auto &f:realised.roofs)if(!f.gable && !f.soffit)addRoof(f.polygon);
    for(const auto &d:realised.dormers)for(const auto &f:d.roofFaces)addRoof(f);
    covered.clear();g::unionOf(operands,covered,&error);area=0;for(const auto &p:covered)area+=g::areaMetres(p);
    std::cout<<"ROOF_REALISED rooms="<<realised.rooms.size()<<" faces="<<realised.roofs.size()<<" dormers="<<realised.dormers.size()<<" covered="<<area<<"\n";
    check(realisedOK && std::abs(area-g::areaMetres(footprint))<.001,"dormer aperture replacement preserves complete roof coverage");
    if(argc>1 && std::string(argv[1])=="--roof-probe")return failed ? 1 : 0;
  }
  {
    namespace gen=procedural::world::generator;
    gen::CellHouse house;house.grid=procedural::geom2::CellGrid(8,8);house.frame.cellMetres=1;house.gridClass.storeyMetres=3;
    gen::CellFloor ground,upper;ground.area=house.grid.mask();upper.area=ground.area;upper.level=1;
    for(uint32_t i=3;i<5;++i)for(uint32_t j=3;j<5;++j)upper.area.clear(i,j);
    house.floors={ground,upper};gen::CellRoofs roofs;gen::Ledger ledger;gen::Stream stream(17);
    const bool built=gen::planRoofs(house,{},stream,roofs,ledger,&error);
    double area=0,top=0;for(const auto &f:roofs.faces)if(f.wing==0 && !f.gable && !f.soffit) {
      const auto a=f.polygon[0],b=f.polygon[1],c=f.polygon[2];area+=std::abs((b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x))*.5;
      for(auto p:f.polygon)top=std::max(top,p.y);
    }
    check(built && std::abs(area-4)<1e-6,"occupied roof pocket bounded by taller walls remains fully roofed");
    check(top>3 && top<=6,"enclosed roof ridge stays below the surrounding upper wall head");
    house.floors.front().area=upper.area;
    const bool courtBuilt=gen::planRoofs(house,{},stream,roofs,ledger,&error);
    check(courtBuilt && std::none_of(roofs.faces.begin(),roofs.faces.end(),[](const auto &f){return f.wing==0;}),"a genuine courtyard remains open instead of receiving the pocket cap");
  }
  {
    CityCompletionPlan completion;completion.revision=1;
    completion.skeleton.regions.push_back({991,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::WorkerHousing,{{0,0,0},{12,0,0},{12,0,12},{0,0,12}},false,144});
    CityLotDevelopment lot;lot.lot=991;lot.preferredFootprintMetres2=32;lot.desiredRooms=2;lot.minimumFloors=lot.maximumFloors=1;
    completion.lots.push_back(lot);CityModelPlan model;SettlementFabric fabric;
    const bool built=buildCityModel(completion,fabric,{},model,&error);
    check(built && model.buildings.size()==1,"small poor home builds on its allocated lot");
    CityModelSettings infrastructure;infrastructure.infrastructureOnly=true;
    CityModelPlan ground;
    const bool groundBuilt=buildCityModel(completion,fabric,infrastructure,ground,&error);
    check(groundBuilt && ground.buildings.empty() && !ground.mesh.triangles.empty(),"infrastructure mode retains lot terrain without dwelling meshes");
    bool weights=groundBuilt;
    for(const auto &triangle:ground.groundWeights)for(const auto &w:triangle)
      weights&=std::abs(w[0]+w[1]+w[2]-1)<1e-8;
    check(weights && ground.groundWeights.size()==ground.mesh.triangles.size(),"infrastructure terrain carries normalized texture weights");
    if(built && !model.buildings.empty()) {
      const auto &b=model.buildings.front();double area=0;for(size_t k=0;k<b.footprint.size();++k){auto a=b.footprint[k],c=b.footprint[(k+1)%b.footprint.size()];area+=a.x*c.z-c.x*a.z;}
      check(b.windows<=1 && std::abs(area)*.5<=32.001 && b.entranceRise==0,"poor home remains small, ground-level and has at most one window");
    }
  }
  {
    namespace gen=procedural::world::generator;
    namespace g=procedural::geom2;
    gen::CellHouse house;house.grid=g::CellGrid(10,10);house.frame.cellMetres=1;
    gen::CellFloor floor;for(uint32_t i=3;i<8;++i)for(uint32_t j=2;j<6;++j)floor.area.set(i,j);
    house.floors.push_back(floor);house.entrance=house.grid.cellEdge(house.grid.cell(3,3),g::Dir::North);
    gen::CellDwellingBrief brief;brief.lot.outer={{0,0},{g::toUnit(10),0},{g::toUnit(10),g::toUnit(10)},{0,g::toUnit(10)}};brief.frontage={0};
    gen::CellDwellingSettings settings;settings.entranceFlightCells=1;
    gen::planCellDwellingSite(brief,settings,house);
    check(house.siteAccess.layout==EntranceLayout::Sideways && house.siteAccess.rise>0,"sideways entrance reserves a single frontage row on the dwelling grid");
    check(house.siteAccess.rise<=.54+1e-8,"short stair runs retain the 18 cm maximum riser limit");
    bool clear=true;for(auto c:house.siteAccess.reservedCells)clear &= !house.grid.contains(floor.area,c);
    check(clear && !house.siteAccess.reservedCells.empty(),"entrance landing, flight and approach use only unoccupied lot cells");
    settings.sidewaysEntrance=false;gen::planCellDwellingSite(brief,settings,house);
    check(house.siteAccess.layout==EntranceLayout::Inset,"inset entrance uses its own grid flight and level landing");
    house.site.obstacle=house.grid.mask() & ~floor.area;gen::planCellDwellingSite(brief,settings,house);
    check(house.siteAccess.rise==0 && house.siteAccess.reservedCells.empty(),"blocked entrance stays at ground level rather than clipping an obstacle");
  }
  {
    CityCompletionPlan completion;completion.revision=1;
    CitySmoothedRoute a;a.id=11;a.halfWidth=2;a.path={{-10,0,0},{10,0,0}};
    CitySmoothedRoute b;b.id=12;b.halfWidth=1;b.path={{0,0,-10},{0,0,10}};
    completion.routes={a,b};CityModelPlan model;SettlementFabric fabric;
    const bool built=buildCityModel(completion,fabric,{},model,&error);
    check(built,"crossing roads build one polygon arrangement before meshing");
    if(built){
      std::vector<procedural::geom2::Operand> polygons;double sum=0;
      for(const auto &s:model.surfaces.pieces)if(s.kind==CitySurfaceKind::Road || s.kind==CitySurfaceKind::Sidewalk){sum+=procedural::geom2::areaMetres(s.polygon);polygons.push_back(procedural::geom2::toOperand(s.polygon));}
      std::vector<procedural::geom2::Polygon> merged;const bool united=procedural::geom2::unionOf(polygons,merged,&error);double area=0;for(const auto &p:merged)area+=procedural::geom2::areaMetres(p);
      check(united && std::abs(area-sum)<1e-6,"road junction has no overlapping surface polygons");
      double meshArea=0;for(auto t:model.mesh.triangles){const auto a=model.mesh.vertices[t[0]],b=model.mesh.vertices[t[1]],c=model.mesh.vertices[t[2]];meshArea+=std::abs((b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x))*.5;}
      check(std::abs(meshArea-sum)<.001,"road triangulation conserves the unified polygon area");
      check(model.roadOwners.size()==model.mesh.triangles.size() && std::count(model.roadOwners.begin(),model.roadOwners.end(),0)==0,"unified road mesh retains segment ownership for later splitting");
      bool normalized=model.groundWeights.size()==model.mesh.triangles.size();
      for(size_t i=0;i<model.groundWeights.size();++i)for(const auto &w:model.groundWeights[i]) {
        const double expected=1.;
        normalized&=std::isfinite(w[0]+w[1]+w[2]) && std::abs(w[0]+w[1]+w[2]-expected)<1e-8 && *std::min_element(w.begin(),w.end())>=0;
      }
      check(normalized,"terrain vertex weights are finite, nonnegative and normalized");
    }
  }
  {
    namespace g=procedural::geom2;
    CityCompletionPlan city;city.revision=1;
    city.skeleton.regions.push_back({100,1,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Residential,{{-10,0,-10},{10,0,-10},{10,0,10},{-10,0,10}},false,400});
    city.skeleton.regions.push_back({200,2,100,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::WorkerHousing,{{-12,0,2},{4,0,2},{4,0,8},{-12,0,8}},false,96});
    city.skeleton.regions.push_back({201,3,100,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Merchant,{{0,0,3},{8,0,3},{8,0,9},{0,0,9}},false,48});
    CitySmoothedRoute road;road.id=1;road.halfWidth=2;road.path={{-10,0,0},{10,0,0}};road.groundCover=CityGroundCover::Dirt;city.routes.push_back(road);
    CitySurfaceStructure surfaces;const bool built=buildCitySurfaces(city,surfaces,&error);
    check(built,"street-first block and lot ground allocation builds");
    if(built) {
      std::vector<g::Operand> operands;double sum=0;bool contained=true;
      for(const auto &p:surfaces.pieces){sum+=g::areaMetres(p.polygon);operands.push_back(g::toOperand(p.polygon));
        if(p.kind==CitySurfaceKind::Lot)for(auto v:p.polygon.outer)contained&=std::abs(g::toMetres(v.x))<=10.0001 && std::abs(g::toMetres(v.y))<=10.0001;
      }
      std::vector<g::Polygon> united;bool valid=g::unionOf(operands,united,&error);double area=0;for(const auto &p:united)area+=g::areaMetres(p);
      check(valid && std::abs(sum-area)<1e-5,"street, block and overlapping lots emit no stacked ground area");
      check(contained,"lot surfaces are clipped to their allocated parent block");
      const auto core=cityGroundWeights(surfaces,0,0),edge=cityGroundWeights(surfaces,0,2),outside=cityGroundWeights(surfaces,0,4);
      check(core[1]>.99 && edge[1]>.45 && edge[1]<.55 && outside[0]>.99,"road core feathers continuously into grass at its polygon boundary");
      const auto left=cityGroundWeights(surfaces,0,1.999),right=cityGroundWeights(surfaces,0,2.001);
      check(std::abs(left[1]-right[1])<.01,"shared ground boundary samples have no material jump");
      std::reverse(city.skeleton.regions.begin(),city.skeleton.regions.end());CitySurfaceStructure reordered;
      check(buildCitySurfaces(city,reordered,&error) && reordered.pieces.size()==surfaces.pieces.size(),"surface ownership is independent of region input order");
    }
  }
  {
    namespace gen=procedural::world::generator;
    gen::CompoundBrief brief;brief.id=810;brief.seed=17;brief.kind=gen::CompoundKind::Monastery;
    brief.precinct={{0,0,0},{64,0,0},{64,0,56},{0,0,56}};brief.approach={32,0,0};
    gen::CompoundPlan monastery;gen::Ledger ledger;
    const auto flat=[](double,double){return 0.;};
    const bool planned=gen::planCompound(brief,flat,monastery,ledger,&error);
    check(planned && monastery.wings.size()==5 && monastery.courts.size()==1,"monastery allocates five wings and an open cloister court");
    if(planned)
    {
      SettlementFabric fabric;fabric.compounds.push_back(monastery);CityModelPlan model;
      CityCompletionPlan completion;completion.revision=1;
      check(buildCityModel(completion,fabric,{},model,&error),"monastery wings pass through cell blueprint, skeleton and mesh");
      check(model.buildings.size()==5 && model.structuralRooms>5 && model.compoundTriangles>100,
            "monastery retains real rooms and structural roof/wall geometry");
      check(std::any_of(model.buildings.begin(),model.buildings.end(),[](const auto &b){return b.chapels>0 && b.chancels>0;}),"monastery church contains a side chapel and chancel");
      check(std::all_of(model.buildings.begin(),model.buildings.end(),[](const auto &b){return b.kind==CityModelBuildingKind::Religious;}),
            "monastery service and residential wings retain religious compound identity");
      const auto &church=monastery.wings.front().brief.shell;
      const auto a=church[0],b=church[1];
      const double len=std::hypot(b.x-a.x,b.z-a.z),nx=-(b.z-a.z)/len,nz=(b.x-a.x)/len;
      CitySmoothedRoute road;road.id=1;road.halfWidth=1.5;
      road.path={{a.x+nx,0,a.z+nz},{b.x+nx,0,b.z+nz}};
      completion.routes.push_back(road);
      const bool replanned=buildCityModel(completion,fabric,{},model,&error);
      check(replanned,"compound blueprint replans around a road intruding into its nominal wing lot");
      if(replanned)
      {
        const auto &footprint=model.buildings.front().footprint;
        double low=1e100,high=-1e100;
        for(auto p:footprint) {const double d=(p.x-road.path[0].x)*nx+(p.z-road.path[0].z)*nz;low=std::min(low,d);high=std::max(high,d);}
        check(low>=road.halfWidth || high<=-road.halfWidth,"replanned church footprint stays outside the complete road width");
      }
    }
    brief.precinct={{0,0,0},{20,0,0},{20,0,20},{0,0,20}};
    check(!gen::planCompound(brief,flat,monastery,ledger,&error),"undersized monastery precinct requests upstream fallback");
  }
  {
    using procedural::geom3::Vertex;
    const std::vector<std::pair<double,double>> outline={{0,0},{6,0},{6,4},{4,4},{4,2},{2,2},{2,4},{0,4}};
    for (int variant=0;variant<3;++variant) {
      std::vector<Vertex> face;
      for (auto [x,z]:outline) face.push_back(variant==2 ? Vertex{5,x,z} : Vertex{x,3+.5*x+.25*z,z});
      if (variant==1) std::reverse(face.begin(),face.end());
      std::vector<std::array<uint32_t,3>> triangles;
      bool valid=procedural::geom3::triangulateFace(face,triangles,&error);
      double area=0;
      const auto uv=[&](Vertex p){ return std::pair<double,double>{variant==2 ? p.y : p.x,p.z}; };
      const auto cross=[](auto a,auto b,auto p){ return (b.first-a.first)*(p.second-a.second)-(b.second-a.second)*(p.first-a.first); };
      for (auto t:triangles) {
        const auto a=uv(face[t[0]]),b=uv(face[t[1]]),c=uv(face[t[2]]);
        const double signedArea=cross(a,b,c);
        area+=std::abs(signedArea)*.5;
        valid &= variant==1 ? signedArea<0 : signedArea>0;
        const std::pair<double,double> notch{3,3};
        const double x=cross(a,b,notch),y=cross(b,c,notch),z=cross(c,a,notch);
        valid &= !((x>=0 && y>=0 && z>=0) || (x<=0 && y<=0 && z<=0));
      }
      check(valid && std::abs(area-20)<1e-8,"concave roof face preserves notch, area and winding on sloped/reversed/vertical planes");
    }
  }
  {
    CityCompletionPlan completion;completion.revision=1;
    CitySmoothedRoute road;road.id=1;road.halfWidth=2;road.path={{-5,0,0},{25,0,0}};road.groundCover=CityGroundCover::Stone;completion.routes.push_back(road);
    for(uint64_t side=0;side<2;++side)for(uint64_t j=0;j<2;++j) {
      const uint64_t id=200+side*2+j;const double x=j*7.,z=side?2.:-16.;
      completion.skeleton.regions.push_back({id,id,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Craftsmen,
        {{x,0,z},{x+7,0,z},{x+7,0,z+14},{x,0,z+14}},false,98});
      CityLotDevelopment lot;lot.lot=id;lot.rowHouse=true;lot.minimumFloors=lot.maximumFloors=3;lot.desiredRooms=3;lot.preferredFootprintMetres2=75;completion.lots.push_back(lot);
    }
    CityModelPlan model;const bool built=buildCityModel(completion,{},{},model,&error);
    bool doors=built && model.buildings.size()==4,projected=false,flush=false;
    for(const auto &b:model.buildings) {
      const bool north=b.id>=202;
      double front=north?1e100:-1e100;
      for(auto p:b.footprint)front=north?std::min(front,p.z):std::max(front,p.z);
      bool onFront=false;for(auto p:b.exteriorDoors)onFront|=std::abs(p.z-front)<.25;
      doors&=onFront;
      double total=0;for(auto d:b.jettyMetres)total+=d;
      projected|=total>=.79;flush|=total==0;
    }
    check(doors,"both sides of a street retain usable frontage doors after road clipping");
    check(projected && flush,"city row houses mix strong legal jetties with flush upper floors");
    const auto paved=cityGroundWeights(model.surfaces,3,2.7);
    check(paved[2]>.9,"allocated frontage reserves stone paving between street and house");
  }
  for (double angle:{0.,.47,.7853981633974483})
  {
    CityCompletionPlan completion;
    completion.revision=1;
    for (uint64_t id=1;id<=2;++id) {
      const double x=(id-1)*5.3;
      completion.skeleton.regions.push_back({id,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Residential,
        {{x,0,0},{x+5.3,0,0},{x+5.3,0,12.4},{x,0,12.4}},false,65.72});
      CityLotDevelopment lot; lot.lot=id; lot.rowHouse=true; lot.maximumFloors=1; lot.preferredFootprintMetres2=65.72;
      completion.lots.push_back(lot);
    }
    for (auto &r:completion.skeleton.regions) for (auto &p:r.boundary) {
      const double x=p.x,z=p.z;
      p.x=100+x*std::cos(angle)-z*std::sin(angle); p.z=200+x*std::sin(angle)+z*std::cos(angle);
    }
    CityModelPlan terraces;
    const bool ok=buildCityModel(completion,{},{},terraces,&error);
    bool joined=ok && terraces.buildings.size()==2;
    for (const auto &b:terraces.buildings) {
      unsigned shared=0;
      for (auto p:b.footprint) shared+=std::abs((p.x-100)*std::cos(angle)+(p.z-200)*std::sin(angle)-5.3)<.002;
      joined=joined && shared==2 && b.wallHeight<=3.01;
    }
    check(joined,"row-house blueprints fill non-integral frontage widths and meet at one-storey party walls");
  }
  {
    CityCompletionPlan completion;
    completion.revision=1;
    CityWallTowerStructure tower;
    tower.id=1; tower.radius=2.5; tower.height=8;
    completion.towers.push_back(tower);
    SettlementFabric fabric;
    procedural::world::generator::CompoundPlan compound;
    compound.id=2; compound.kind=procedural::world::generator::CompoundKind::Castle;
    procedural::world::generator::CompoundWing wing;
    wing.role="keep"; wing.brief.id=3; wing.brief.floors=3;
    wing.brief.shell={{10,0,10},{26,0,10},{26,0,26},{10,0,26}};
    // Intentionally no legacy plan: allocation, not that plan's success, is
    // the input to the modern cell pipeline.
    compound.wings.push_back(wing);
    wing.role="tower"; compound.wings.push_back(wing);
    fabric.compounds.push_back(compound);
    CityModelPlan model;
    check(buildCityModel(completion,fabric,{},model,&error),"city compounds and towers use structural generators");
    bool floorsAboveGround=true;size_t roomBuildings=0;
    for(const auto &building:model.buildings)if(building.rooms){++roomBuildings;floorsAboveGround &= building.base>fabric.terraceLevel+.035;}
    check(roomBuildings>0 && floorsAboveGround,"finished room floors sit above the shared ground surface skin");
    check(model.buildings.size()==2 && model.compoundTriangles>100 && model.towerTriangles>100 &&
          model.structuralRooms>0 && model.structuralContacts>0 &&
          std::count(model.materials.begin(),model.materials.end(),42)>0,
          "city retains compound rooms and roofs without duplicate square tower wings");
    // Old compound plans carried floor slabs without the matching cell wall
    // and roof topology. A stale slab must never leak into the city product.
    DwellingFloor staleFloor;
    staleFloor.slabs={{{-80,0,-80},{80,0,-80},{80,0,80},{-80,0,80}}};
    fabric.compounds.front().wings.front().plan.floors.push_back(staleFloor);
    fabric.compounds.front().wings.front().planned=true;
    CityModelPlan fromStalePlan;
    const bool rebuilt=buildCityModel(completion,fabric,{},fromStalePlan,&error);
    bool identical=rebuilt && model.materials==fromStalePlan.materials &&
      model.mesh.triangles==fromStalePlan.mesh.triangles && model.mesh.vertices.size()==fromStalePlan.mesh.vertices.size();
    for (size_t i=0;identical && i<model.mesh.vertices.size();++i) {
      const auto a=model.mesh.vertices[i],b=fromStalePlan.mesh.vertices[i];
      identical=a.x==b.x && a.y==b.y && a.z==b.z;
    }
    check(identical,"stale compound floor plates cannot leak into the regenerated city mesh");
    // Deliberately overlapping ownership polygons must be resolved before
    // dwelling generation, including civic/compound reservations.
    auto collision=completion;
    for (uint64_t id=100;id<102;++id) {
      const double x=id==100 ? 18 : 30;
      collision.skeleton.regions.push_back({id,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Residential,
        {{x,0,10},{x+20,0,10},{x+20,0,30},{x,0,30}},false,400});
      CityLotDevelopment lot; lot.lot=id; lot.preferredFootprintMetres2=60; lot.desiredRooms=6;
      collision.lots.push_back(lot);
    }
    CityModelPlan separated;
    check(buildCityModel(collision,fabric,{},separated,&error) && separated.lotMeshes==2,
          "overlapping lots fit both homes around compound and neighbour reservations");
    bool disjoint=true;
    for (size_t i=0;i<separated.buildings.size();++i)
      for (size_t j=i+1;j<separated.buildings.size();++j) {
        std::vector<procedural::geom2::Operand> operands;
        for (auto k:{i,j}) {
          procedural::geom2::Polygon p;
          for (auto v:separated.buildings[k].footprint) p.outer.push_back({procedural::geom2::toUnit(v.x),procedural::geom2::toUnit(v.z)});
          if (!procedural::geom2::counterClockwise(p.outer)) procedural::geom2::reverse(p.outer);
          operands.push_back(procedural::geom2::toOperand(p));
        }
        std::vector<procedural::geom2::Polygon> intersection;
        disjoint &= procedural::geom2::intersectionOf(operands,intersection,nullptr);
        for (const auto &p:intersection) disjoint &= std::abs(procedural::geom2::areaMetres(p))<1e-6;
      }
    check(disjoint,"reserved blueprints have no positive-area building footprint intersections");
  }
  {
    CityModelPlan model;
    // A sloped roof and a vertical wall cross two tile boundaries.
    model.mesh.vertices={{-1,1,-1},{3,3,-1},{1,2,3},{-1,0,.5},{3,0,.5},{1,4,.5}};
    model.mesh.triangles={{0,1,2},{3,4,5}};
    model.mesh.expectedNormals={{-0.4472135955,0.894427191,0},{0,0,1}};
    model.materials={14,12};
    model.surfaceMaterials={2,1};
    for(auto t:model.mesh.triangles) {
      std::array<std::array<double,2>,3> uv;
      for(size_t k=0;k<3;++k){auto p=model.mesh.vertices[t[k]];uv[k]={p.x+p.z,p.y+p.z};}
      model.triangleUVs.push_back(uv);
      CityModelPlan::GroundWeights weights;
      for(size_t k=0;k<3;++k){const auto p=model.mesh.vertices[t[k]];weights[k]={(p.x+1)/8.,(p.z+1)/8.,1-(p.x+p.z+2)/8.};}
      model.groundWeights.push_back(weights);
    }
    const auto area=[](const SplineFaceMesh &m) {
      double sum=0;
      for (auto t:m.triangles) {
        const auto a=m.vertices[t[0]],b=m.vertices[t[1]],c=m.vertices[t[2]];
        const double x=(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y);
        const double y=(b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z);
        const double z=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
        sum+=.5*std::hypot(x,std::hypot(y,z));
      }
      return sum;
    };
    double covered=0;
    bool bounded=true,attributes=true;
    for (int x=0;x<2;++x) for (int z=0;z<2;++z) {
      SplineFaceMesh tile;
      std::vector<uint32_t> materials;
      std::vector<uint32_t> surfaceMaterials;
      std::vector<std::array<std::array<double,2>,3>> uvs;
      std::vector<CityModelPlan::GroundWeights> groundWeights;
      const double loX=-2+3*x,loZ=-2+3*z;
      appendCityModelTile(model,loX,loZ,loX+3,loZ+3,tile,materials,&uvs,&surfaceMaterials,&groundWeights);
      covered+=area(tile);
      for (const auto p:tile.vertices) bounded &= p.x>=loX && p.x<=loX+3 && p.z>=loZ && p.z<=loZ+3;
      attributes &= tile.triangles.size()==materials.size() && materials.size()==tile.expectedNormals.size();
      attributes &= uvs.size()==tile.triangles.size() && surfaceMaterials.size()==materials.size();
      for(size_t ti=0;ti<tile.triangles.size();++ti)for(size_t k=0;k<3;++k) {
        auto p=tile.vertices[tile.triangles[ti][k]];
        attributes &= std::abs(uvs[ti][k][0]-p.x-p.z)<1e-9 && std::abs(uvs[ti][k][1]-p.y-p.z)<1e-9;
        attributes &= surfaceMaterials[ti]==(materials[ti]==14?2u:1u);
        attributes &= std::abs(groundWeights[ti][k][0]-(p.x+1)/8.)<1e-9 &&
          std::abs(groundWeights[ti][k][1]-(p.z+1)/8.)<1e-9 &&
          std::abs(groundWeights[ti][k][0]+groundWeights[ti][k][1]+groundWeights[ti][k][2]-1)<1e-9;
      }
    }
    check(bounded && attributes && std::abs(covered-area(model.mesh))<1e-9,
          "city tile clipping preserves crossing surfaces, bounds, normals, materials and interpolated UV seams");
  }
  for (int z = 0; z < 3; ++z)
    for (int x = 0; x < 3; ++x)
    {
      CityPlanCell cell;
      cell.id = z * 3 + x + 1;
      cell.boundary = {{x * 20., 0, z * 20.}, {x * 20. + 20, 0, z * 20.}, {x * 20. + 20, 0, z * 20. + 20}, {x * 20., 0, z * 20. + 20}};
      city.cells.push_back(cell);
    }
  city.gates = {1};
  city.districts = {{1, DistrictUse::Residential, 400, 800, true}};
  GenerationSnapshot outputs;
  check(
      runGenerators({cityMasterPlanGenerator(1)}, {{{"city.plan-brief", 1}, {city, 1}}}, 7, outputs, {}, &error) && !std::any_cast<const CityMasterPlan &>(outputs.at({"city.master-plan", 1}).value).districts.empty(),
      "native city allocation through common interface");
  RuntimeBuildInput runtime;
  runtime.dependencies = {1, 1, 1, 1, 1};
  runtime.map.minX = 0;
  runtime.map.minZ = 0;
  runtime.map.maxX = 32;
  runtime.map.maxZ = 32;
  runtime.map.cutsX = {16};
  runtime.map.cutsZ = {16};
  runtime.terrain = [](double, double)
  {
    return 0.;
  };
  runtime.errors.resize(1);
  runtime.densities.resize(1);
  runtime.covers = {{0, {0}, false, false}};
  SurfaceContour ground;
  ground.id = 1;
  ground.loop = {{0, 0, 0}, {32, 0, 0}, {32, 0, 32}, {0, 0, 32}};
  runtime.producers = {{1, 0, {ground}}};
  check(runGenerators({runtimeWorldGenerator(1)}, {{{"runtime.brief", 1}, {runtime, 1}}}, 7, outputs, {}, &error), "native world builder executes through common interface");
  if (outputs.count({"runtime.candidate", 1}))
  {
    auto &assets = std::any_cast<const RuntimeBuildResult &>(outputs.at({"runtime.candidate", 1}).value).assets;
    WorldRuntimePublication publication;
    uint64_t ticket = 0;
    bool ok = publication.begin(assets, std::vector<uint32_t>(assets.levels[0].world.tiles.size(), 0), 1000000, ticket, &error);
    if (ok)
      for (uint32_t i = 0; i < assets.levels[0].world.tiles.size(); ++i)
        ok &= publication.complete(ticket, i, assets.dependencies, &error);
    auto snapshot = publication.acquire();
    RuntimeSurfaceHit hit;
    check(ok && snapshot && queryRuntimeSurface(*snapshot, 16, 16, 0, hit, &error) && hit.height == 0, "DAG world candidate publishes and serves real collision/surface queries");
  }
  // W0: actual-world settlement input adapter on a generated world.
  {
    procedural::world::WorldPipelineSettings settings;
    settings.brief.extentMetres = 12000;
    settings.brief.seed = 17;
    settings.brief.reliefMetres = 900;
    settings.brief.tags = {procedural::world::Tag::Coast, procedural::world::Tag::Lowland};
    settings.cellSpacingMetres = 400;
    settings.hierarchyLevels = 1;
    auto world = std::make_shared<procedural::world::GeneratedWorld>();
    bool generated = procedural::world::generateWorldDirected(settings, *world, {}, &error);
    check(generated, "actual 12 km world generates for settlement integration");
    WorldSiteSelectionSettings siteSettings;
    WorldSettlementSite site;
    bool selected = generated && selectWorldSettlementSite(*world, siteSettings, site, &error);
    check(selected, "W0 site selection composes a brief from the actual world");
    if (selected)
    {
      size_t rejected = 0;
      for (auto &c : site.candidates)
        rejected += !c.accepted;
      std::cout << "W0_SITE cell=" << site.cell << " region=\"" << site.regionName << "\" centre=(" << site.centre.x << "," << site.centre.z << ") elevation=" << site.centre.y << " candidates=" << site.candidates.size()
                << " rejected=" << rejected << " attempts=" << site.attempts << " planningCells=" << site.brief.input.cells.size() << " regions=" << site.regions.size() << " gates=" << site.brief.input.gateCells.size()
                << " sources=" << site.brief.input.ruralSources.size() << " districts=" << site.brief.input.districts.size() << " origin=(" << site.origin.x << "," << site.origin.z << ") size=" << site.bounds.maxX << "x"
                << site.bounds.maxZ << " openArea=" << site.openArea << " hinterlandArea=" << site.hinterlandArea << "\n";
      size_t shown = 0;
      for (auto &c : site.candidates)
        if (!c.accepted && shown++ < 6)
          std::cout << "  rejected cell " << c.cell << ": " << c.rejection << "\n";
      for (auto &r : site.rejections)
        std::cout << "  omission: " << r << "\n";
      double lo = 1e300, hi = -1e300;
      for (auto &cell : site.brief.input.cells)
      {
        double y = site.brief.terrain(cell.boundary[0].x, cell.boundary[0].z);
        lo = std::min(lo, y);
        hi = std::max(hi, y);
      }
      check(hi > lo, "terrain callback samples the actual surface rather than a flat default");
      check(!site.water(site.localCentre.x, site.localCentre.z), "site centre is not on explicit map water");
      bool waterSeen = false, waterExplicit = true;
      for (const auto &n : world->graph.nodes())
        if (!n.refined() && !n.land && n.site.x - site.origin.x > site.bounds.minX && n.site.x - site.origin.x < site.bounds.maxX && n.site.y - site.origin.z > site.bounds.minZ &&
            n.site.y - site.origin.z < site.bounds.maxZ)
        {
          waterSeen = true;
          waterExplicit &= site.water(n.site.x - site.origin.x, n.site.y - site.origin.z);
        }
      check(!waterSeen || waterExplicit, "explicit ocean/lake cells inside the domain read as water");
      WorldSettlementSite repeat;
      bool same = selectWorldSettlementSite(*world, siteSettings, repeat, &error) && repeat.id == site.id && repeat.cell == site.cell && repeat.candidates.size() == site.candidates.size() &&
                  repeat.brief.input.cells.size() == site.brief.input.cells.size() && repeat.regions.size() == site.regions.size() && repeat.brief.input.gateCells == site.brief.input.gateCells &&
                  repeat.brief.suitability.fingerprint() == site.brief.suitability.fingerprint();
      for (size_t i = 0; same && i < site.candidates.size(); ++i)
        same &= site.candidates[i].cell == repeat.candidates[i].cell && site.candidates[i].rejection == repeat.candidates[i].rejection;
      for (size_t i = 0; same && i < site.regions.size(); ++i)
        same &= site.regions[i].id == repeat.regions[i].id && site.regions[i].cover == repeat.regions[i].cover;
      for (size_t i = 0; same && i < site.brief.input.cells.size(); ++i)
        same &= site.brief.input.cells[i].id == repeat.brief.input.cells[i].id;
      check(same, "same seed yields the same site, candidate ledger, region and cell IDs");
      uint32_t waterCell = procedural::world::kInvalidNode;
      for (const auto &n : world->graph.nodes())
        if (!n.refined() && !n.land)
        {
          waterCell = n.id;
          break;
        }
      auto bad = siteSettings;
      bad.preferredCell = waterCell;
      WorldSettlementSite rejectedSite;
      bool refused = waterCell != procedural::world::kInvalidNode && !selectWorldSettlementSite(*world, bad, rejectedSite, &error) && !error.empty();
      std::cout << "  explicit water cell " << waterCell << " -> " << error << "\n";
      check(refused, "a preferred water cell is rejected with a visible reason");
      CityTerritory territory;
      bool composed = generateCityTerritory(site.brief.input, site.brief.suitability, site.brief.settings, site.brief.terrain, territory, &error);
      std::string territoryMessage = "W0 territory allocates districts and farmland on actual terrain: " + error;
      check(composed, territoryMessage.c_str());
      if (composed)
      {
        std::cout << "W0_TERRITORY districts=" << territory.regions.districts.size() << " omissions=" << territory.regions.omissions.size() << " urbanArea=" << territory.regions.assignedArea << " patches=" << territory.rural.patches.size()
                  << " cropArea=" << territory.cropArea << " farms=" << territory.holdings.size() << " contours=" << territory.surfaces.contours.size() << " eligibleArea=" << territory.rural.eligibleArea
                  << " ruralRejections=" << territory.rural.rejections.size() << "\n";
        for (auto &o : territory.regions.omissions)
          std::cout << "  district omission " << o.request << ": " << o.reason << "\n";
        size_t ruralShown = 0;
        for (auto &r : territory.rural.rejections)
          if (ruralShown++ < 40)
            std::cout << "  rural: " << r << "\n";
        {
          const auto &w = site.brief.suitability;
          const char *names[] = {"Wet", "Flood", "Cliff", "Talus", "Brink", "Steep", "Rough", "Inaccessible", "Ramp", "PoorSoil", "Saturated"};
          std::vector<size_t> histogram(11, 0);
          size_t eligible = 0, walkable = 0;
          for (auto &c : w.cells())
          {
            eligible += c.reasons == 0;
            walkable += c.walkable;
            for (int b = 0; b < 11; ++b)
              histogram[b] += (c.reasons >> b) & 1;
          }
          std::cout << "  suitability cells=" << w.cells().size() << " eligible=" << eligible << " walkable=" << walkable;
          for (int b = 0; b < 11; ++b)
            if (histogram[b])
              std::cout << " " << names[b] << "=" << histogram[b];
          std::cout << "\n";
          size_t regionShown = 0;
          for (auto &region : site.regions)
          {
            if (region.cover != RuralLandCover::Open || regionShown++ >= 8)
              continue;
            std::vector<size_t> h(11, 0);
            size_t cells = 0, blocked = 0, noAccess = 0;
            double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
            for (auto p : region.boundary)
            {
              x0 = std::min(x0, p.x);
              z0 = std::min(z0, p.z);
              x1 = std::max(x1, p.x);
              z1 = std::max(z1, p.z);
            }
            for (size_t i = 0; i < w.cells().size(); ++i)
            {
              auto c = w.cells()[i].center;
              if (c.x < x0 || c.x > x1 || c.z < z0 || c.z > z1)
                continue;
              ++cells;
              blocked += w.cells()[i].reasons != 0;
              noAccess += !territory.rural.catchments[i];
              for (int b = 0; b < 11; ++b)
                h[b] += (w.cells()[i].reasons >> b) & 1;
            }
            std::cout << "  region " << region.id << " cells=" << cells << " blocked=" << blocked << " noCatchment=" << noAccess;
            for (int b = 0; b < 11; ++b)
              if (h[b])
                std::cout << " " << names[b] << "=" << h[b];
            std::cout << "\n";
          }
        }
        check(!territory.regions.districts.empty(), "population-derived districts are allocated to actual planning cells");
      }
      // Fabric (terrace, streets, houses, residents) and W1 runtime publication.
      if (composed)
      {
        SettlementFabricSettings fabricSettings;
        SettlementFabric fabric;
        bool fabricOK = composeSettlementFabric(site, territory, fabricSettings, fabric, &error);
        std::string fabricMessage = "settlement fabric composes on the actual territory: " + error;
        check(fabricOK, fabricMessage.c_str());
        if (fabricOK)
        {
          std::cout << "W1_FABRIC terrace=" << (fabric.terraceAccepted ? "accepted" : "rejected") << " level=" << fabric.terraceLevel << " step=" << fabric.terraceStep << " polygon=" << fabric.polygon.outer.size() << " holes=" << fabric.polygon.holes.size()
                    << " routeCells=" << fabric.routeCells << " arteries=" << fabric.arteries.size() << " gates=" << fabric.gates.size() << " layout=" << (fabric.layoutAccepted ? "accepted" : "rejected") << " streets=" << fabric.layout.streets.size()
                    << " objects=" << fabric.layout.objects.size() << " populated=" << fabric.populated << " housed=" << fabric.population.housed << " unhoused=" << fabric.population.unhoused << " buildings=" << fabric.population.buildings.size()
                    << " structures=" << fabric.structures.triangles.size() << " farmTriangles=" << fabric.farmTriangles << "\n";
          for (auto &r : fabric.rejections)
            std::cout << "  fabric: " << r << "\n";
          {
            size_t vertices = 0, cityVertices = 0, houses = 0, gardens = 0, gaps = 0;
            for (auto &c : territory.surfaces.contours)
              vertices += c.loop.size();
            for (auto &c : fabric.layout.contours)
              cityVertices += c.loop.size();
            for (auto &o : fabric.layout.objects)
              o.kind == CityObjectKind::Garden ? ++gardens : o.kind == CityObjectKind::RejectedGap ? ++gaps : ++houses;
            std::cout << "  territory contours=" << territory.surfaces.contours.size() << " vertices=" << vertices << " city contours=" << fabric.layout.contours.size() << " vertices=" << cityVertices << " houses=" << houses << " gardens=" << gardens
                      << " gaps=" << gaps << " polygonArea=" << [&] { double a = 0; auto &p = fabric.polygon.outer; for (size_t i = 0; i < p.size(); ++i) a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x; return a * .5; }() << "\n";
            for (auto &c : territory.surfaces.contours)
              if (c.loop.size() < 3 || c.loop.size() > 256)
                std::cout << "  bad contour id=" << c.id << " material=" << c.material << " role=" << int(c.role) << " vertices=" << c.loop.size() << "\n";
          }
          check(fabric.terraceAccepted && fabric.layoutAccepted && !fabric.layout.objects.empty(), "village streets and houses are laid out on the bounded terrace");
          check(fabric.populated && fabric.population.housed > 0, "residents are housed through the director with real terrain/water eligibility");
          RuntimeBuildInput runtimeInput;
          SettlementRuntimeSettings runtimeSettings;
          bool briefOK = composeSettlementRuntimeInput(site, territory, fabric, runtimeSettings, runtimeInput, &error);
          std::string briefMessage = "W1 runtime brief composes from site, territory and fabric: " + error;
          check(briefOK, briefMessage.c_str());
          RuntimeBuildResult built;
          bool buildOK = briefOK && buildRuntimeWorld(runtimeInput, nullptr, built, &error);
          // Runtime publication is parked (user direction 2026-09-13): report,
          // but only require an explicit message when it does not build.
          std::string buildMessage = "W1 runtime build either succeeds or reports an explicit failure: " + error;
          if (!buildOK && !error.empty())
          {
            std::cout << "  runtime (parked): " << error << "\n";
            buildOK = true;
          }
          if (!buildOK)
          {
            struct Worst { double deviation, length; uint64_t producer, id; uint32_t material; int role; size_t edge; };
            std::vector<Worst> worst;
            for (auto &batch : runtimeInput.producers)
              for (auto &c : batch.contours)
              {
                if (!c.terrainSurface)
                  continue;
                for (size_t i = 0; i < c.loop.size(); ++i)
                {
                  auto a = c.loop[i], b = c.loop[(i + 1) % c.loop.size()];
                  double ya = runtimeInput.terrain(a.x, a.z), yb = runtimeInput.terrain(b.x, b.z), len = std::hypot(b.x - a.x, b.z - a.z), dev = 0;
                  for (double t : {.25, .5, .75})
                    dev = std::max(dev, std::abs(runtimeInput.terrain(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t) - (ya + (yb - ya) * t)));
                  if (dev > .2)
                    worst.push_back({dev, len, batch.producer, c.id, c.material, int(c.role), i});
                }
              }
            std::sort(worst.begin(), worst.end(), [](auto &x, auto &y) { return x.deviation > y.deviation; });
            std::cout << "  edges over 0.2 m: " << worst.size() << " cutsX=" << runtimeInput.map.cutsX.size() << " cutsZ=" << runtimeInput.map.cutsZ.size() << "\n";
            {
              WorldFinalPatches frozen;
              SurfaceRefinement initial = runtimeInput.initial;
              initial.samplingSeed = runtimeInput.samplingSeed;
              std::string freezeError;
              if (freezeWorldPatches(runtimeInput.producers, runtimeInput.map, initial, runtimeInput.terrain, runtimeInput.revision, frozen, &freezeError))
              {
                for (auto &face : frozen.map.faces)
                {
                  bool hasA = false, hasB = false;
                  for (auto vi : face.vertices)
                  {
                    auto v = frozen.map.vertices[vi];
                    hasA |= std::abs(v.x - 2400) < .01 && std::abs(v.z - 2552) < .01;
                    hasB |= std::abs(v.x - 3040) < .01 && std::abs(v.z - 2552) < .01;
                  }
                  if (hasA && hasB)
                  {
                    std::cout << "  face " << face.id << " material " << face.material << " source " << face.sourceContour << " ring:";
                    for (auto vi : face.vertices)
                      std::cout << " (" << frozen.map.vertices[vi].x << "," << frozen.map.vertices[vi].z << ")";
                    std::cout << "\n";
                  }
                }
              }
              else
                std::cout << "  freeze: " << freezeError << "\n";
            }
            for (auto &batch : runtimeInput.producers)
              for (auto &c : batch.contours)
                for (size_t i = 0; i < c.loop.size(); ++i)
                  if (std::abs(c.loop[i].z - 2552) < .5 && c.loop[i].x > 2000)
                    std::cout << "  vertex near z=2552: producer=" << batch.producer << " contour=" << c.id << " material=" << c.material << " role=" << int(c.role) << " terrain=" << c.terrainSurface << " size=" << c.loop.size() << " v=(" << c.loop[i].x << "," << c.loop[i].z << ")\n";
            for (size_t i = 0; i < worst.size() && i < 6; ++i)
              std::cout << "  edge producer=" << worst[i].producer << " contour=" << worst[i].id << " material=" << worst[i].material << " role=" << worst[i].role << " edge=" << worst[i].edge << " length=" << worst[i].length << " deviation=" << worst[i].deviation << "\n";
          }
          check(buildOK, buildMessage.c_str());
          if (buildOK && built.assets.levels.empty())
            std::cout << "W1_RUNTIME built with no levels; publication checks skipped\n";
          if (buildOK && !built.assets.levels.empty())
          {
            const auto &assets = built.assets;
            std::cout << "W1_RUNTIME tiles=" << assets.levels[0].world.tiles.size() << " faces=" << assets.levels[0].world.map.faces.size() << " triangles=" << assets.levels[0].world.mesh.triangles.size() << " structures=" << assets.structures.triangles.size()
                      << " covers=" << assets.coverDefinitions.size() << " changedTiles=" << built.changedTiles.size() << "\n";
            WorldRuntimePublication publication;
            uint64_t ticket = 0;
            bool published = publication.begin(assets, std::vector<uint32_t>(assets.levels[0].world.tiles.size(), 0), 4000000, ticket, &error);
            for (uint32_t i = 0; published && i < assets.levels[0].world.tiles.size(); ++i)
              published &= publication.complete(ticket, i, assets.dependencies, &error);
            auto snapshot = publication.acquire();
            RuntimeSurfaceHit street, field;
            Position streetPoint = fabric.layout.streets.front().path.front();
            Position fieldPoint;
            for (auto &f : territory.fields)
              if (!f.fields.empty())
              {
                for (auto v : f.fields.front().boundary)
                {
                  fieldPoint.x += v.x / f.fields.front().boundary.size();
                  fieldPoint.z += v.z / f.fields.front().boundary.size();
                }
                break;
              }
            // Ground just outside the terrace, inside the core job.
            fieldPoint = {runtimeInput.map.minX + 5, 0, runtimeInput.map.minZ + 5};
            bool streetOK = published && snapshot && queryRuntimeSurface(*snapshot, streetPoint.x, streetPoint.z, 0, street, &error) && std::abs(street.height - fabric.terraceLevel) < 1e-3;
            bool fieldOK = published && snapshot && queryRuntimeSurface(*snapshot, fieldPoint.x, fieldPoint.z, 0, field, &error) && std::abs(field.height - site.brief.terrain(fieldPoint.x, fieldPoint.z)) < .5;
            std::cout << "  street query " << street.height << " vs terrace " << fabric.terraceLevel << "; ground query " << field.height << " vs terrain " << site.brief.terrain(fieldPoint.x, fieldPoint.z) << " " << error << "\n";
            check(streetOK, "published street surface agrees with the terrace the houses were laid out on");
            check(fieldOK, "published ground surface agrees with the actual world terrain");
            RuntimeBuildInput fullInput;
            SettlementRuntimeSettings fullSettings;
            fullSettings.domain = SettlementRuntimeDomain::Full;
            RuntimeBuildResult fullBuilt;
            bool fullOK = composeSettlementRuntimeInput(site, territory, fabric, fullSettings, fullInput, &error) && buildRuntimeWorld(fullInput, nullptr, fullBuilt, &error);
            std::cout << "  full-domain job: " << (fullOK ? "built" : error) << "\n";
            check(fullOK || error.find("budget") != std::string::npos || error.find("stations") != std::string::npos, "full hinterland job either builds or reports an explicit capacity failure");
          }
        }
      }
      GenerationSnapshot chain;
      WorldSiteGenerationBrief brief{world, siteSettings};
      SettlementFabricSettings chainFabric;
      SettlementRuntimeSettings chainRuntime;
      bool ran = runGenerators(
          {worldSettlementSiteGenerator(1), settlementTerritoryBriefGenerator(1), cityTerritoryGenerator(1), settlementFabricGenerator(1), settlementRuntimeBriefGenerator(1), runtimeWorldGenerator(1)},
          {{{"world.site-brief", 1}, {brief, 1}}, {{"settlement.fabric-settings", 1}, {chainFabric, 1}}, {{"settlement.runtime-settings", 1}, {chainRuntime, 1}}},
          17,
          chain,
          {},
          &error);
      if (!ran)
        std::cout << "  runtime chain (parked): " << error << "\n";
      check(ran || error.find("preserved") != std::string::npos || error.find("budget") != std::string::npos, "site -> territory -> fabric -> runtime chain executes or reports an explicit runtime failure");
      // Class-based generators: the same chain as generator objects sharing a
      // ledger, with the hinterland published through per-tile runtime jobs.
      {
        using namespace procedural::world::generator;
        auto ledger = std::make_shared<Ledger>();
        SettlementRuntimeSettings tiled;
        tiled.domain = SettlementRuntimeDomain::Full;
        GenerationSnapshot classed;
        bool classRan = runGenerators(settlementGeneratorTasks(1, ledger), {{{"world.site-brief", 1}, {brief, 1}}, {{"settlement.fabric-settings", 1}, {chainFabric, 1}}, {{"settlement.runtime-settings", 1}, {tiled, 1}}}, 17, classed, {}, &error);
        std::string classMessage = "class-based settlement generators run through the director: " + error;
        if (!classRan)
          std::cout << "  class chain (parked runtime): " << error << "\n";
        check(classRan || error.find("no runtime job built") != std::string::npos, classMessage.c_str());
        std::cout << "GENERATOR_LEDGER rejected=" << ledger->count(LedgerKind::Rejected) << " omitted=" << ledger->count(LedgerKind::Omitted) << " capacity=" << ledger->count(LedgerKind::Capacity) << " repaired=" << ledger->count(LedgerKind::Repaired) << "\n";
        if (classRan && classed.count({"runtime.results", 1}))
        {
          const auto &results = std::any_cast<const RuntimeJobResults &>(classed.at({"runtime.results", 1}).value);
          const auto &fabricProduct = std::any_cast<const SettlementFabric &>(classed.at({"settlement.fabric", 1}).value);
          const auto &territoryProduct = std::any_cast<const CityTerritory &>(classed.at({"city.territory", 1}).value);
          size_t faces = 0;
          for (const auto &job : results.jobs)
          {
            if (job.built)
              faces += job.result.assets.levels[0].world.map.faces.size();
            std::cout << "  job " << job.name << " [" << job.minX << "," << job.minZ << "," << job.maxX << "," << job.maxZ << "] " << (job.built ? "faces=" + std::to_string(job.result.assets.levels[0].world.map.faces.size()) : job.error) << "\n";
          }
          std::cout << "W1_TILED jobs=" << results.jobs.size() << " built=" << results.built << " faces=" << faces << "\n";
          // The first field's centroid must publish in the tile that holds it.
          Position fieldPoint;
          bool fieldFound = false;
          for (auto &f : territoryProduct.fields)
            if (!f.fields.empty() && !fieldFound)
            {
              for (auto v : f.fields.front().boundary)
              {
                fieldPoint.x += v.x / f.fields.front().boundary.size();
                fieldPoint.z += v.z / f.fields.front().boundary.size();
              }
              fieldFound = true;
            }
          bool fieldPublished = false, fieldTileBuilt = false;
          for (const auto &job : results.jobs)
            if (fieldFound && fieldPoint.x >= job.minX && fieldPoint.x <= job.maxX && fieldPoint.z >= job.minZ && fieldPoint.z <= job.maxZ)
            {
              fieldTileBuilt = job.built;
              if (!job.built || job.result.assets.levels.empty())
                break;
              WorldRuntimePublication publication;
              uint64_t ticket = 0;
              const auto &assets = job.result.assets;
              bool published = publication.begin(assets, std::vector<uint32_t>(assets.levels[0].world.tiles.size(), 0), 4000000, ticket, &error);
              for (uint32_t i = 0; published && i < assets.levels[0].world.tiles.size(); ++i)
                published &= publication.complete(ticket, i, assets.dependencies, &error);
              auto snapshot = publication.acquire();
              RuntimeSurfaceHit hit;
              fieldPublished = published && snapshot && queryRuntimeSurface(*snapshot, fieldPoint.x, fieldPoint.z, 0, hit, &error) && std::abs(hit.height - fabricProduct.terrain(fieldPoint.x, fieldPoint.z)) < .5;
              std::cout << "  field query " << hit.height << " vs terrain " << fabricProduct.terrain(fieldPoint.x, fieldPoint.z) << " in job " << job.name << "\n";
              break;
            }
          check(fieldFound && fieldTileBuilt && fieldPublished, "a farm field publishes through its tile job and agrees with the world terrain");
        }
      }
    }
  }
  // A1 jettying acceptance: eligibility, exclusions, clearance and limits.
  {
    JettyRequest poorRow;
    poorRow.floors = 2;
    poorRow.streetWidthMetres = 6;
    // The mix is deterministic in (seed, house): find one projecting and one flush slot among the first houses.
    uint64_t projecting = 0, flushSlot = 0;
    for (uint64_t house = 1; house <= 16 && (!projecting || !flushSlot); ++house)
    {
      poorRow.house = house;
      auto trial = decideJetty(poorRow);
      if (trial.jettied && !projecting)
        projecting = house;
      if (trial.eligible && !trial.jettied && !flushSlot)
        flushSlot = house;
    }
    poorRow.house = projecting;
    auto row = decideJetty(poorRow);
    JettyRequest shop = poorRow;
    shop.floors = 3;
    auto shopHouse = decideJetty(shop);
    JettyRequest flushRow = poorRow;
    flushRow.house = flushSlot;
    auto flush = decideJetty(flushRow);
    JettyRequest wealthy = poorRow;
    wealthy.poorOrMiddleHousehold = false;
    JettyRequest church = poorRow;
    church.institutional = true;
    JettyRequest narrow = poorRow;
    narrow.streetWidthMetres = 2.6;
    JettyRequest opposing = poorRow;
    opposing.streetWidthMetres = 3.6;
    opposing.oppositeJettyMetres = .6; // 3.6 - .6 - 2.5 = .5 available.
    auto conflict = decideJetty(opposing);
    bool cumulative = shopHouse.jettied && std::abs(shopHouse.total - .9) < 1e-9 && shopHouse.perFloor[1] == .45 && shopHouse.perFloor[2] == .45;
    check(row.jettied && std::abs(row.total - .6) < 1e-9 && row.perFloor[0] == 0, "A1: eligible poor row house projects its upper floor by 0.6 m");
    check(cumulative, "A1: three-floor shop-house keeps the per-floor and cumulative limits");
    check(projecting && flushSlot && flush.eligible && !flush.jettied, "A1: flush-front variant by deterministic mix");
    check(!decideJetty(wealthy).eligible && !decideJetty(church).eligible, "A1: wealthy and institutional buildings are excluded");
    check(decideJetty(narrow).eligible && !decideJetty(narrow).jettied, "A1: narrow street rejects the projection");
    check(conflict.jettied && std::abs(conflict.total - .5) < 1e-9, "A1: opposing overhang reduces the projection to keep the corridor");
    DwellingBrief jettyBrief;
    jettyBrief.id = 9;
    jettyBrief.use = DwellingUse::ShopHouse;
    jettyBrief.floors = 2;
    jettyBrief.availableHeight = 6;
    jettyBrief.shell = {{0, 0, 0}, {9, 0, 0}, {9, 0, 11}, {0, 0, 11}};
    jettyBrief.entrance = {4.5, 0, 0};
    jettyBrief.jettyMetres = {0, .6};
    DwellingPlan jettied;
    bool planned = generateDwellingPlan(jettyBrief, jettied, &error);
    double front = 1e9;
    for (auto &f : jettied.floors)
      if (&f == &jettied.floors.back())
        for (auto &slab : f.slabs)
          for (auto v : slab)
            front = std::min(front, v.z);
    check(planned && !jettied.fallback && jettied.floors.size() == 2 && std::abs(front + .6) < 1e-6 && jettied.floors[0].grossArea < jettied.floors[1].grossArea, "A1: jettied upper floor polygon projects 0.6 m over a fixed ground footprint");
  }
  // B1-B3 block grammar on a synthetic hillside block: every lot reachable,
  // alleys connected to the street, stairs where the alley climbs.
  {
    using namespace procedural::world::generator;
    ConvexRing block{{0, 0, 0}, {60, 0, 0}, {60, 0, 40}, {0, 0, 40}};
    std::vector<bool> streets{true, false, true, false}; // South and north sides face streets.
    BlockGrammarSettings settings;
    settings.seed = 17;
    Ledger blockLedger;
    BlockPlan plan;
    bool ok = planBlockGrammar(5, block, streets, [](double, double z) { return z * .12; }, settings, plan, blockLedger, &error);
    std::cout << "BLOCK_GRAMMAR cells=" << plan.cells.size() << " lots=" << plan.lots << " alleys=" << plan.alleys << " stairs=" << plan.stairs << " courts=" << plan.courts << " streets=" << plan.streets << " inaccessible=" << plan.inaccessibleLots << " restarts=" << plan.restarts << " " << error << "\n";
    check(ok && plan.cells.size() > 20 && plan.lots > 0 && plan.streets > 0, "block grammar tiles a block into quads with street, lot and alley cells");
    check(ok && plan.solved, "every lot has alley or street access and dead alleys became courts");
    check(ok && plan.stairs > 0, "alleys climbing the hillside step become stairs");
    BlockPlan again;
    Ledger againLedger;
    bool same = planBlockGrammar(5, block, streets, [](double, double z) { return z * .12; }, settings, again, againLedger, &error) && again.cells.size() == plan.cells.size();
    for (size_t i = 0; same && i < plan.cells.size(); ++i)
      same &= again.cells[i].tile == plan.cells[i].tile;
    check(same, "block grammar is deterministic in its seed");
  }
  // A2 compounds: a castle and a manor on synthetic precincts.
  {
    using namespace procedural::world::generator;
    Ledger compoundLedger;
    CompoundBrief castle;
    castle.id = 3;
    castle.kind = CompoundKind::Castle;
    castle.precinct = {{0, 0, 0}, {70, 0, 0}, {74, 0, 52}, {-2, 0, 56}};
    castle.approach = {35, 0, 0};
    CompoundPlan castlePlan;
    bool castleOK = planCompound(castle, [](double x, double z) { return z * .02 + x * .01; }, castlePlan, compoundLedger, &error);
    std::cout << "COMPOUND castle walls=" << castlePlan.walls.size() << " towers=" << castlePlan.towers.size() << " wings=" << castlePlan.wings.size() << " planned=" << castlePlan.plannedWings << " " << error << "\n";
    for (auto &w : castlePlan.wings)
      std::cout << "  wing " << w.role << " " << (w.planned ? "planned rooms=" + std::to_string(w.plan.rooms.size()) + " floors=" + std::to_string(w.plan.floors.size()) : w.note) << "\n";
    bool gateOnApproachSide = castleOK && std::hypot(castlePlan.gate.x - castle.approach.x, castlePlan.gate.z - castle.approach.z) < 14;
    check(castleOK && castlePlan.walls.size() == 6 && castlePlan.towers.size() == 5 && gateOnApproachSide, "A2: castle has a closed curtain wall with corner towers and a gatehouse on the approach side");
    check(castleOK && castlePlan.solved && castlePlan.wings.size() >= 2, "A2: keep, hall and stable wings are planned interiors");
    CompoundBrief manor;
    manor.id = 4;
    manor.kind = CompoundKind::Manor;
    manor.precinct = {{0, 0, 0}, {40, 0, 0}, {40, 0, 30}, {0, 0, 30}};
    manor.approach = {20, 0, 0};
    CompoundPlan manorPlan;
    bool manorOK = planCompound(manor, [](double, double) { return 0.; }, manorPlan, compoundLedger, &error);
    std::cout << "COMPOUND manor walls=" << manorPlan.walls.size() << " wings=" << manorPlan.wings.size() << " planned=" << manorPlan.plannedWings << " " << error << "\n";
    for (auto &w : manorPlan.wings)
      std::cout << "  wing " << w.role << " " << (w.planned ? "planned rooms=" + std::to_string(w.plan.rooms.size()) : w.note) << "\n";
    check(manorOK && manorPlan.solved && manorPlan.wings.size() == 2 && manorPlan.walls.size() == 6, "A2: manor plans a hall wing and a service wing around a walled court");
  }
  std::vector<GenerationTask> catalogue{
    landPartitionGenerator(1),
    coastDetailGenerator(1),
    riverNetworkGenerator(1),
    watershedGenerator(1),
    channelGenerator(1),
    roadNetworkGenerator(1),
    roadEarthworkGenerator(1),
    cityDemandGenerator(1),
    cityMasterPlanGenerator(1),
    cityLayoutGenerator(1),
    ruralPatchesGenerator(1),
    cityTerritoryGenerator(1),
    settlementProgrammeGenerator(1),
    runtimeWorldGenerator(1),
    farmFieldsGenerator(1),
    dwellingPlanGenerator(1),
    dwellingMeshGenerator(1),
    worldSettlementSiteGenerator(1),
    settlementTerritoryBriefGenerator(1),
    settlementFabricGenerator(1),
    settlementRuntimeBriefGenerator(1)};
  bool valid = true;
  for (auto &task : catalogue)
    valid &= !task.descriptor.id.empty() && !task.descriptor.reads.empty() && task.descriptor.writes.size() == 1;
  check(valid && catalogue.size() == 21, "all twenty-one native domain adapters expose typed contracts");
  if (argc > 1)
  {
    std::ofstream file(argv[1]);
    auto write = [&](const std::vector<GenerationTask> &tasks)
    {
      file << "[";
      bool first = true;
      for (auto &task : tasks)
      {
        if (!first)
          file << ",";
        first = false;
        file << "{\"id\":" << std::quoted(task.descriptor.id) << ",\"domain\":" << task.descriptor.domain << ",\"parallelSafe\":" << (task.parallelSafe ? "true" : "false");
        for (auto field : {false, true})
        {
          file << (field ? ",\"writes\":[" : ",\"reads\":[");
          bool initial = true;
          for (auto &port : field ? task.descriptor.writes : task.descriptor.reads)
          {
            if (!initial)
              file << ",";
            initial = false;
            file << "{\"product\":" << std::quoted(port.key.product) << ",\"schema\":" << port.schema << "}";
          }
          file << "]";
        }
        file << "}";
      }
      file << "]";
    };
    file << "{\"schema\":1,\"activeMapDAG\":";
    write(procedural::world::worldPipelineGenerators());
    file << ",\"nativeAdapters\":";
    write(catalogue);
    file << "}\n";
  }
  std::cout << checks << " checks, " << failed << " failures\n";
  return failed ? 1 : 0;
}
