#include "procedural/world/WorldCityRampTerrain.hpp"
#include "procedural/world/WorldCityStreetProfiles.hpp"
#include "procedural/world/WorldCityFacetTerrain.hpp"
#include "procedural/world/WorldCityOccupiedStreets.hpp"
#include "procedural/world/WorldCityRoadPaint.hpp"
#include "procedural/world/WorldCityElevationContours.hpp"
#include "procedural/world/WorldCityContourTerrain.hpp"
#include "procedural/world/WorldCityBase.hpp"
#include "procedural/world/WorldCityDistrictUpgrade.hpp"
#include "procedural/world/generator/SettlementGenerators.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include <cmath>
#include <iostream>
using namespace procedural::world::rewrite;
namespace {
bool inside(const std::vector<Position> &ring,Position p) {
  bool in=false;for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++) {
    const auto a=ring[j],b=ring[i];
    if((a.z>p.z)!=(b.z>p.z) && p.x<(b.x-a.x)*(p.z-a.z)/(b.z-a.z)+a.x)in=!in;
  }return in;
}
}
int main() {
  int failures=0;const auto check=[&](bool ok,const char *name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<'\n';failures+=!ok;};
  {
    CityCompletionPlan city;
    CitySmoothedRoute a;a.id=1;a.halfWidth=3;a.path={{-14,0,0},{14,0,0}};
    auto b=a;b.id=2;for(auto &p:b.path)p.z+=.05;
    city.routes={a,b};CitySurfaceStructure surface;std::string why;
    bool covered=false;
    const bool built=buildCitySurfaces(city,surface,&why);
    for(const auto &piece:surface.pieces)if(piece.kind==CitySurfaceKind::Road) {
      std::vector<Position> ring;
      for(auto p:piece.polygon.outer)ring.push_back({procedural::geom2::toMetres(p.x),0,procedural::geom2::toMetres(p.y)});
      covered |= inside(ring,{0,0,3.025});
    }
    check(built && covered,"thin road overlap partitions retain paving instead of becoming grass slots");
    city.routes.clear();city.skeleton.alleys={{{0,0,0},{12,0,0},{12,0,2},{0,0,2}}};
    const bool alleyBuilt=buildCitySurfaces(city,surface,&why);
    bool pedestrian=alleyBuilt && !surface.pieces.empty();
    for(const auto &piece:surface.pieces)pedestrian &= piece.kind==CitySurfaceKind::Sidewalk;
    check(pedestrian,"block alleys remain pedestrian paving without an inset miniature carriageway");
  }

  {
    CityCompletionPlan quarter;std::string why;
    const bool planned=planCityStreetQuarter(41,quarter,&why);
    check(planned && quarter.lots.size()==12 && quarter.routes.size()==1,
          "compact street quarter allocates two frontage rows around one curved public lane");
    bool frontage=planned;
    for(const auto &r:quarter.skeleton.regions)if(r.kind==CityRegionKind::Lot)
      frontage &= r.publicFrontage.size()==2 && r.courtFacing && r.area>=60;
    check(frontage,"street-quarter homes carry explicit public frontage and usable floor area");
    CityCompletionPlan repeat;planCityStreetQuarter(41,repeat,&why);
    check(repeat.routes.front().path[20].z==quarter.routes.front().path[20].z,
          "street-quarter circulation is deterministic");
    SettlementFabric fabric;CityModelSettings serial,batched;serial.maximumWorkers=1;serial.maximumResidentLots=1;
    CityModelPlan a,b;
    bool same=buildCityModel(quarter,fabric,serial,a,&why) && buildCityModel(quarter,fabric,batched,b,&why);
    same &= a.mesh.vertices.size()==b.mesh.vertices.size() && a.mesh.triangles==b.mesh.triangles &&
      a.surfaceMaterials==b.surfaceMaterials && a.triangleUVs==b.triangleUVs;
    if(same)for(size_t i=0;i<a.mesh.vertices.size();++i) {
      auto p=a.mesh.vertices[i],q=b.mesh.vertices[i];same &= p.x==q.x && p.y==q.y && p.z==q.z;
    }
    check(same && a.buildings.size()==12,"bounded lot batches preserve the serial street-quarter mesh");
    CityReliefPlan relief;
    relief.ground=relief.openGround=[](double x,double z){return .085*x+.03*z+.12*std::sin(x*.1)*std::sin(z*.2);};
    planCityHillBlocks(quarter,relief,{});CityModelPlan hill;
    bool clear=buildCityModel(quarter,fabric,batched,hill,&why,&relief);size_t treads=0;
    for(const auto &building:hill.buildings)for(const auto &slab:building.siteAccess.slabs)if(slab.tread) {
      ++treads;
      for(size_t e=0;e<slab.polygon.size();++e)for(int sample=0;sample<=8;++sample) {
        auto a=slab.polygon[e],b=slab.polygon[(e+1)%slab.polygon.size()];
        Position p{a.x+(b.x-a.x)*sample/8.,0,a.z+(b.z-a.z)*sample/8.};
        for(const auto &road:quarter.routes)for(size_t k=1;k<road.path.size();++k) {
          auto a=road.path[k-1],b=road.path[k];double dx=b.x-a.x,dz=b.z-a.z;
          double t=std::clamp(((p.x-a.x)*dx+(p.z-a.z)*dz)/(dx*dx+dz*dz),0.,1.);
          clear &= std::hypot(p.x-a.x-t*dx,p.z-a.z-t*dz)>=std::max(.9,road.halfWidth-batched.streetPavementMetres)-.01;
        }
      }
    }
    check(clear && hill.buildings.size()==12 && treads>12,"all hillside entrances retain homes and keep their full stair width outside the carriageway");
    CityReliefPlan::RaisedBlock cut;cut.block=300;
    cut.boundary={{44,0,-6},{58,0,-6},{58,0,10},{44,0,10}};
    cut.level=4.15;cut.terrainCut=true;cut.stairHalfWidth=1.2;
    cut.accessFrom={38,relief.openGround(38,0),0};cut.accessTo={44,cut.level,0};
    const double outside=relief.openGround(60,8),uncut=relief.openGround(56,8);
    bool excavated=planRaisedCityBlocks(quarter.skeleton,{cut},{},relief,&why);
    check(excavated && uncut>cut.level && relief.openGround(56,8)<=cut.level-.119 &&
          relief.ground(56,8)<=cut.level-.119 && relief.openGround(60,8)==outside,
          "cut terraces excavate both terrain samplers beneath their deck and preserve surrounding hills");
    check(excavated && !relief.stairs.empty() && std::abs(relief.stairs.back().b.y-cut.level)<1e-6,
          "cut terrace stairs connect to the planned finished terrace elevation");
  }
  {
    CitySkeleton skeleton;skeleton.revision=1;
    skeleton.roadNodes={{1,CityRoadNodeKind::Endpoint,{0,0,0},{}},
      {2,CityRoadNodeKind::Junction,{100,0,0},{}},{3,CityRoadNodeKind::Endpoint,{150,0,0},{}}};
    CityRoadEdge a;a.id=1;a.from=1;a.to=2;a.path={{0,0,0},{100,0,0}};a.halfWidth=2;
    CityRoadEdge b;b.id=2;b.from=2;b.to=3;b.path={{100,0,0},{150,0,0}};b.halfWidth=2;
    skeleton.roadEdges={a,b};
    auto valley=[](double x,double){return -2*std::max(0.,1-std::abs(x-50)/10.);};
    CityReliefSettings settings;settings.maximumStreetGrade=.08;
    CityStreetProfilePlan plan;std::string why;
    const bool ok=planCityStreetProfiles(skeleton,valley,settings,plan,&why);
    check(ok && std::abs(plan.junctions[1]+.335)<1e-8 && std::abs(plan.junctions[2]+.335)<1e-8,
          "local valley no longer lowers distant junctions or a whole neighbourhood");
    bool safe=ok;size_t corners=0;
    if(ok)for(const auto &street:plan.streets) {
      safe &= street.maximumGrade<=.0800001;corners+=street.path.size();
      for(size_t i=1;i<street.path.size();++i)for(unsigned j=0;j<=50;++j) {
        const auto a=street.path[i-1],b=street.path[i];double t=j/50.;
        const double x=a.x+(b.x-a.x)*t,z=a.z+(b.z-a.z)*t,y=a.y+(b.y-a.y)*t;
        safe &= y+.035<=valley(x,z)-settings.streetExcavationMetres+1e-7;
      }
    }
    check(safe && corners<20 && plan.stationCount>100,
          "station planner preserves excavation and grades while simplifying straight mesh runs");
    double localCut=0,wholeRoadCut=0;
    if(ok)for(unsigned x=0;x<100;++x) {
      const double sample=x+.5;double level=0;bool found=false;
      for(size_t k=1;k<plan.streets[0].path.size();++k) {
        const auto a=plan.streets[0].path[k-1],b=plan.streets[0].path[k];
        if(sample<a.x || sample>b.x)continue;
        level=a.y+(b.y-a.y)*(sample-a.x)/(b.x-a.x);found=true;break;
      }
      safe &= found;localCut+=valley(sample,0)-level-.035;
      wholeRoadCut+=valley(sample,0)+2.3;
    }
    check(ok && safe && localCut<wholeRoadCut*.65,
          "local street profiles materially reduce excavation compared with whole-road lowering");
    check(ok && plan.streets[0].path.back().y==plan.streets[1].path.front().y,
          "street blueprints share exact junction elevation");
    std::reverse(skeleton.roadEdges.begin(),skeleton.roadEdges.end());CityStreetProfilePlan repeat;
    bool deterministic=planCityStreetProfiles(skeleton,valley,settings,repeat,&why) && repeat.streets.size()==plan.streets.size();
    if(deterministic)for(size_t i=0;i<plan.streets.size();++i) {
      deterministic &= repeat.streets[i].edge==plan.streets[i].edge && repeat.streets[i].path.size()==plan.streets[i].path.size();
      if(deterministic)for(size_t j=0;j<plan.streets[i].path.size();++j)
        deterministic &= repeat.streets[i].path[j].y==plan.streets[i].path[j].y;
    }
    check(deterministic,"street blueprint is independent of edge input order");
    const auto before=repeat.stationCount;
    check(!planCityStreetProfiles(skeleton,[](double,double){return NAN;},settings,repeat,&why) && repeat.stationCount==before,
          "invalid terrain fails street blueprint transactionally");
  }
  {
    // A steep hillside, with two nearby streets whose cut/fill banks overlap.
    CityCompletionPlan city;city.revision=city.skeleton.revision=1;
    CitySkeletonRegion block;block.id=1;block.kind=CityRegionKind::Block;
    block.boundary={{-10,0,-15},{110,0,-15},{110,0,25},{-10,0,25}};
    city.skeleton.regions.push_back(block);
    for(unsigned i=0;i<2;++i) {
      CityRoadNode a,b;a.id=10+i*2;b.id=a.id+1;
      a.position={0,0,i*8.};b.position={100,0,i*8.};
      city.skeleton.roadNodes.push_back(a);city.skeleton.roadNodes.push_back(b);
      CityRoadEdge edge;edge.id=20+i;edge.from=a.id;edge.to=b.id;edge.halfWidth=2;
      edge.path={a.position,b.position};city.skeleton.roadEdges.push_back(edge);
      CitySmoothedRoute route;route.id=edge.id;route.edges={edge.id};route.path=edge.path;route.halfWidth=2;
      city.routes.push_back(route);
    }
    SettlementFabric fabric;fabric.terrain=[](double x,double z){return .4*x+.3*z;};
    CityLandformCell cell;cell.id=1;cell.boundary=block.boundary;cell.centre={50,0,5};
    fabric.landforms.cells.push_back(cell);
    CityReliefSettings options;options.terrainFacetMetres=0;options.maximumStreetGrade=.08;
    CityReliefPlan relief;std::string why;
    const bool planned=planCityRelief(city.skeleton,fabric.terrain,options,relief,&why);
    bool level=planned && relief.maximumGrade<=.080001,continuous=planned;
    if(planned)for(const auto &e:relief.edges)for(double t:{.2,.5,.8}) {
      level &= e.grade<=options.maximumStreetGrade+1e-8;
      const auto a=e.path.front(),b=e.path.back();const double x=a.x+(b.x-a.x)*t,h=a.y+(b.y-a.y)*t;
      for(double dz:{-2.,-1.,0.,1.,2.})level &= std::abs(relief.openGround(x,a.z+dz)-h)<1e-8;
      continuous &= std::abs(relief.openGround(x,a.z+2.000001)-h)<1e-4;
    }
    check(level,"steep streets obey grade limit and stay horizontal across their full width beside another bank");
    check(continuous,"road cut/fill banks meet exact tread edges continuously");
    bool excavated=planned;
    if(planned)for(const auto &e:relief.edges)for(unsigned station=0;station<=100;++station)
      for(double lateral:{-2.,-1.,0.,1.,2.}) {
        const double x=station,z=e.path.front().z+lateral;
        excavated &= relief.openGround(x,z)+.035<=fabric.terrain(x,z)-options.streetExcavationMetres+1e-7;
      }
    check(excavated,"street bed is excavated below natural terrain across the entire road width");
    auto valley=fabric.terrain;
    CityReliefPlan valleyPlan;
    auto valleySource=[valley](double x,double z){return valley(x,z)-12*std::max(0.,1-std::abs(x-50)/10.);};
    bool valleyCut=planCityRelief(city.skeleton,valleySource,options,valleyPlan,&why);
    if(valleyCut)for(const auto &e:valleyPlan.edges)for(unsigned station=0;station<=100;++station)
      for(double lateral:{-2.,0.,2.}) {
        const double x=station,z=e.path.front().z+lateral;
        valleyCut &= valleyPlan.openGround(x,z)+.035<=valleySource(x,z)-options.streetExcavationMetres+1e-7;
      }
    check(valleyCut,"interior terrain valley caps excavation instead of leaving a floating road chord");
    CityModelSettings settings;settings.groundOnly=true;settings.textureStreets=true;settings.streetCarveMetres=0;
    CityModelPlan mesh;const bool built=planned && buildCityModel(city,fabric,settings,mesh,&why,&relief);
    check(built && mesh.construction.revision==city.revision && !mesh.construction.elements.empty(),
          "normal city mesh pipeline carries the construction blueprint candidate");
    size_t roadVertices=0;bool exact=built;
    if(built)for(const auto &p:mesh.mesh.vertices)if(p.x>10 && p.x<90 && std::abs(p.z)<=2.000001) {
      ++roadVertices;exact &= std::abs(p.y-(relief.openGround(p.x,p.z)+.035))<.002;
    }
    if(!built)std::cerr<<why<<'\n';
    check(exact && roadVertices>4 && mesh.roadTriangles==0,
          "contour CDT preserves graded road mesh and road edges with texture-only streets");
    bool interiors=built;
    if(built)for(double x:{25.,50.,75.})for(double z:{-1.5,0.,1.5,6.5,8.,9.5}) {
      bool found=false;
      for(const auto &t:mesh.mesh.triangles) {
        const auto a=mesh.mesh.vertices[t[0]],b=mesh.mesh.vertices[t[1]],c=mesh.mesh.vertices[t[2]];
        const double det=(b.x-a.x)*(c.z-a.z)-(c.x-a.x)*(b.z-a.z);
        if(std::abs(det)<1e-12)continue;
        const double u=((x-a.x)*(c.z-a.z)-(c.x-a.x)*(z-a.z))/det;
        const double v=((b.x-a.x)*(z-a.z)-(x-a.x)*(b.z-a.z))/det;
        if(u< -1e-8 || v< -1e-8 || u+v>1.+1e-8)continue;
        const double rendered=a.y+u*(b.y-a.y)+v*(c.y-a.y);
        found=true;interiors &= std::abs(rendered-relief.openGround(x,z)-.035)<.025;
        interiors &= rendered<=fabric.terrain(x,z)-options.streetExcavationMetres+.025;
      }
      interiors &= found;
    }
    check(interiors,"rendered road triangle interiors are excavated below terrain and follow the graded plane");
  }
  {
    std::vector<CityPlanCell> cells;
    for(unsigned i=0;i<3;++i) {
      CityPlanCell c;c.id=i+1;c.boundary={{i*10.,0,0},{i*10.+10,0,0},{i*10.+10,0,10},{i*10.,0,10}};
      c.defense=i?1.:.1;c.secure=true;cells.push_back(c);
    }
    DistrictRequest house;house.id=1;house.use=DistrictUse::Residential;house.minimumArea=house.targetArea=100;
    DistrictRequest castle;castle.id=2;castle.use=DistrictUse::Castle;castle.minimumArea=castle.targetArea=200;castle.preferredCell=3;
    CityMasterPlan plan;std::string why;
    const bool ok=generateCityMasterPlan(cells,{house,castle},{2},{},plan,&why);
    check(ok,"prepared uphill fortress reserves its full precinct before housing");
    if(ok) {
      auto fort=std::find_if(plan.districts.begin(),plan.districts.end(),[](auto &d){return d.id==2;});
      check(fort!=plan.districts.end()&&fort->anchorCell==3&&fort->cells.size()==2,"summit fortress owns adjacent high cells, not only its anchor");
      std::reverse(cells.begin(),cells.end());CityMasterPlan repeat;
      check(generateCityMasterPlan(cells,{castle,house},{2},{},repeat,&why)&&repeat.inputFingerprint==plan.inputFingerprint,"landmark-first allocation is input-order independent");
    }
  }
  {
    CityCompletionPlan city;city.revision=1;
    CitySkeletonRegion region;region.id=1;region.kind=CityRegionKind::Lot;
    region.boundary={{0,0,0},{18,0,0},{18,0,20},{0,0,20}};region.area=360;
    city.skeleton.regions.push_back(region);
    CityLotDevelopment lot;lot.lot=1;lot.developed=true;city.lots.push_back(lot);
    CitySmoothedRoute street;street.id=2;street.path={{-5,0,-3},{23,0,-3}};street.halfWidth=2;
    city.routes.push_back(street);
    SettlementFabric fabric;fabric.terrain=[](double x,double z){return .04*z+.2*std::exp(-((x-9)*(x-9)+(z-10)*(z-10))/8.);};
    CityModelSettings settings;settings.groundVertexSpacing=2;CityModelPlan model;std::string why;
    const bool ok=buildCityModel(city,fabric,settings,model,&why);
    if(!ok)std::cerr<<why<<'\n';
    check(ok && model.buildings.size()==1,"sloped lot realizes a supported dwelling");
    CityLandformCell cell;cell.id=1;cell.centre={9,0,10};cell.boundary=region.boundary;fabric.landforms.cells.push_back(cell);
    CityModelPlan faceted;
    check(buildCityModel(city,fabric,settings,faceted,&why) && faceted.buildings.size()==1,
          "terrain facet splits preserve the full architectural lot for dwelling fitting");
    if(ok && model.buildings.size()==1) {
      const auto &b=model.buildings.front();
      check(b.base>.8,"finished floor clears the entire lot including its uphill boundary");
      size_t accessFaces=0;
      for(size_t i=0;i<model.mesh.triangles.size();++i)if(model.materials[i]==37) {
        const auto t=model.mesh.triangles[i];const auto a=model.mesh.vertices[t[0]],c=model.mesh.vertices[t[1]],d=model.mesh.vertices[t[2]];
        if(std::abs(a.y-c.y)<1e-6 && std::abs(a.y-d.y)<1e-6 && a.y>.15 && a.y<b.base-.1)++accessFaces;
      }
      check(accessFaces>4,"planned entrance steps are emitted as horizontal native faces");
    }
  }
  {
    CitySkeleton city;CitySkeletonRegion r;r.boundary={{0,0,0},{16,0,0},{16,0,16},{0,0,16}};city.regions.push_back(r);
    const auto source=[](double x,double z){return .03*x*x+.02*z*z;};
    const auto ramp=cityRampTerrain(city,source,8.);
    check(std::abs(ramp(2,2)-(ramp(1,1)+ramp(3,3))*.5)<1e-9,"low-poly hillside is a plane within each facet");
    check(std::abs(ramp(8,8)-source(8,8))<1e-9,"ramps retain sampled landform heights");
    check(std::abs(ramp(4-1e-7,4)-ramp(4+1e-7,4))<1e-5,"adjacent ramp facets meet continuously");
    const auto unchanged=cityRampTerrain(city,source,0.);
    check(unchanged(2,2)==source(2,2),"zero facet pitch preserves the source terrain");
  }
  {
    CityCompletionPlan city;city.revision=city.skeleton.revision=1;
    CitySkeletonRegion r;r.id=1;r.kind=CityRegionKind::Block;r.boundary={{0,0,0},{100,0,0},{100,0,100},{0,0,100}};city.skeleton.regions.push_back(r);
    SettlementFabric fabric;fabric.terrain=[](double x,double){return x*.04;};
    CityModelSettings settings;settings.groundOnly=true;settings.groundVertexSpacing=2;
    CityModelPlan model;std::string why;
    check(buildCityModel(city,fabric,settings,model,&why) && model.mesh.triangles.size()<250,
          "flat ground and straight ramps remain coarse instead of uniform subdivision");
  }
  for(uint64_t seed:{1,17,41,103})for(unsigned tiers:{2,4,6}) {
    CityBaseSettings settings;settings.seed=seed;settings.tiers=tiers;CityBasePlan base;std::string error;
    const bool valid=planCityBase(settings,base,&error);check(valid,"base is planned before any dwelling");
    if(!valid){std::cerr<<error<<'\n';continue;}
    bool compact=true;
    for(const auto &deck:base.relief.platforms) {
      compact &= deck.bridges.empty();
      if(deck.accessPath.size()>=6)compact &= std::abs(deck.accessPath[2].z-deck.accessPath[1].z)-2*deck.stairHalfWidth<=.11;
    }
    check(compact,"switchbacks have compact adjacent flights and no bridge over their stair court");
    check(base.circulation.lots.empty() && base.relief.platforms.size()==tiers,"base contains terraces and circulation but no occupied lots");
    check(base.relief.rocks.size()==tiers*2 && base.relief.fallMetres>5,"hillside has exposed rocks and several distinct levels");
    CityCompletionPlan city;CityReliefPlan relief;
    check(populateCityBase(base,city,relief,&error) && city.lots.size()>tiers*6,"second phase fits dense dwelling lots to reserved envelopes");
    bool clear=true,minimum=true;
    for(const auto &lot:city.skeleton.regions)if(lot.kind==CityRegionKind::Lot) {
      minimum&=lot.area>=55;
      for(const auto &path:city.routes)for(size_t i=1;i<path.path.size();++i) {
        auto a=path.path[i-1],b=path.path[i];double len=std::hypot(b.x-a.x,b.z-a.z);
        for(double d=0;d<=len;d+=.5)for(double side:{-path.halfWidth,0.,path.halfWidth}) {
          Position p{a.x+(b.x-a.x)*d/len-(b.z-a.z)*side/len,0,a.z+(b.z-a.z)*d/len+(b.x-a.x)*side/len};
          clear&=!inside(lot.boundary,p);
        }
      }
    }
    check(clear && minimum,"lots preserve full alley width and minimum useful floor area");
    CityReliefPlan recheck;recheck.ground=recheck.openGround=base.relief.openGround;
    check(planRaisedCityBlocks(city.skeleton,base.relief.platforms,{},recheck,&error),"populated lots preserve stair courts, deck containment and bridge headroom");
    CityBasePlan repeat;check(planCityBase(settings,repeat,&error) && repeat.buildable.size()==base.buildable.size() &&
      repeat.relief.platforms.back().boundary[1].x==base.relief.platforms.back().boundary[1].x,"seeded base is deterministic");
  }
  for(uint64_t seed:{17,41,103})for(auto layout:{CityBaseLayout::Tapered,CityBaseLayout::Staggered,CityBaseLayout::Broad,CityBaseLayout::Voronoi})
  for(auto terrain:{CityBaseTerrain::Hillside,CityBaseTerrain::Ridge,CityBaseTerrain::Valley}) {
    CityBaseSettings settings;settings.seed=seed;settings.layout=layout;settings.terrain=terrain;settings.tiers=3;
    CityBasePlan base;std::string why;
    const bool ok=planCityBase(settings,base,&why);if(!ok)std::cerr<<why<<'\n';
    check(ok,"different block layouts and terrain profiles preserve full stair clearance");
    bool excavated=ok;
    if(ok)for(const auto &stair:base.relief.stairs) {
      double previous=0;const double dx=stair.b.x-stair.a.x,dz=stair.b.z-stair.a.z,len=std::hypot(dx,dz);
      for(auto band:stair.bands) {
        const double along=(previous+band.first)*.5;previous=band.first;
        for(double side:{-stair.halfWidth,0.,stair.halfWidth}) {
          const double x=stair.a.x+dx*along/len-dz*side/len,z=stair.a.z+dz*along/len+dx*side/len;
          excavated &= base.relief.accessGround(x,z)<=band.second-.34 && base.relief.accessGround(x,z)<=base.relief.openGround(x,z)+1e-6;
        }
      }
    }
    check(excavated,"terrain support stays below every tread across the full stair width");
  }
  {
    CityBaseSettings settings;settings.layout=CityBaseLayout::Districts;settings.tiers=2;
    CityBasePlan base;std::string why;
    bool ok=planCityBase(settings,base,&why),curved=false,clear=ok;
    if(ok)for(const auto &route:base.circulation.routes) {
      if(route.path.size()>2) {
        auto a=route.path.front(),b=route.path.back();
        for(auto p:route.path)curved |= std::abs((b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x))>.1;
      }
      for(auto p:route.path)for(const auto &plot:base.buildable) {
        double distance=1e30;
        for(size_t k=0;k<plot.boundary.size();++k) {
          auto a=plot.boundary[k],b=plot.boundary[(k+1)%plot.boundary.size()];
          double dx=b.x-a.x,dz=b.z-a.z,d=dx*dx+dz*dz;
          double t=d?std::clamp(((p.x-a.x)*dx+(p.z-a.z)*dz)/d,0.,1.):0.;
          distance=std::min(distance,std::hypot(p.x-a.x-t*dx,p.z-a.z-t*dz));
        }
        clear &= !inside(plot.boundary,p) && distance+1e-6>=route.halfWidth;
      }
    }
    check(ok && curved && !base.buildable.empty(),"district terrain plans curved lanes before building plots");
    check(clear,"curved street ribbons preserve reserved building clearance");
    settings.laneMeanderMetres=NAN;
    check(!planCityBase(settings,base,&why),"non-finite meander rejected before terrain mutation");
  }
  for(auto layout:{CityBaseLayout::Tapered,CityBaseLayout::Voronoi,CityBaseLayout::Districts}) {
    CityBaseSettings settings;settings.layout=layout;settings.tiers=2;CityBasePlan base;std::string why;
    const bool ok=planCityBase(settings,base,&why);
    SettlementFabric fabric;fabric.terrain=base.relief.openGround;
    CityModelPlan mesh;CityModelSettings options;options.groundVertexSpacing=4;
    const bool built=ok && buildCityModel(base.circulation,fabric,options,mesh,&why,&base.relief);
    bool paving=built;size_t flagstones=0;
    if(built)for(size_t t=0;t<mesh.surfaceMaterials.size();++t)
      if(mesh.surfaceMaterials[t]==18 || mesh.surfaceMaterials[t]==19) {
        ++flagstones;
        for(auto weights:mesh.groundWeights[t])for(auto weight:weights)paving &= weight==0;
      }
    check(paving && flagstones>0,"architectural paving retains flagstone and courtyard materials");
    if(!built)std::cerr<<"Native mesh: "<<why<<'\n';check(built,"native stair court mesh builds");bool clear=built;
    if(built)for(const auto &stair:base.relief.stairs) {
      const double dx=stair.b.x-stair.a.x,dz=stair.b.z-stair.a.z,len=std::hypot(dx,dz);double previous=0;
      for(auto band:stair.bands) {
        const double along=(previous+band.first)*.5;previous=band.first;
        for(double side:{-stair.halfWidth+.2,0.,stair.halfWidth-.2}) {
          Position probe{stair.a.x+dx*along/len-dz*side/len,0,stair.a.z+dz*along/len+dx*side/len};
          double highest=-1e9;
          for(size_t k=0;k<mesh.mesh.triangles.size();++k)if(mesh.materials[k]==37) {
            auto t=mesh.mesh.triangles[k];auto a=mesh.mesh.vertices[t[0]],b=mesh.mesh.vertices[t[1]],c=mesh.mesh.vertices[t[2]];
            if(std::abs(a.y-b.y)>1e-6 || std::abs(a.y-c.y)>1e-6 || a.y>band.second+2.2)continue;
            const auto cross=[](auto a,auto b,auto p){return (b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x);};
            double u=cross(a,b,probe),v=cross(b,c,probe),w=cross(c,a,probe);
            if(std::abs(cross(a,b,c))>1e-8 && ((u>=-1e-6 && v>=-1e-6 && w>=-1e-6)||(u<=1e-6 && v<=1e-6 && w<=1e-6)))highest=std::max(highest,a.y);
          }
          clear &= std::abs(highest-band.second)<1e-5;
        }
      }
    }
    check(clear,"native platform caps and guards leave every tread and landing clear across its width");
    if(ok && layout!=CityBaseLayout::Districts) {
      auto requests=base.relief.platforms;requests.resize(1);
      requests.front().boundary={{-42,0,0},{42,0,0},{42,0,36},{-42,0,36}};
      requests.front().accessTo={requests.front().accessFrom.x,1,36};
      requests.front().accessPath={requests.front().accessFrom,{requests.front().accessFrom.x,0,12},requests.front().accessTo};
      CityReliefPlan bad;bad.ground=bad.openGround=base.relief.openGround;
      check(!planRaisedCityBlocks(base.circulation.skeleton,requests,{},bad,&why) && why.find("solid")!=std::string::npos && bad.platforms.empty(),"invalid access through a solid platform is rejected transactionally");
    }
  }
  {
    CityBaseSettings settings;settings.tiers=2;CityBasePlan base;std::string why;
    const bool planned=planCityBase(settings,base,&why);
    CityReliefPlan relief;relief.openGround=relief.ground=[](double,double){return -1.;};
    CityReliefPlan::Stairway stair;stair.a={0,0,0};stair.b={0,3,12};stair.halfWidth=1;
    stair.bands={{2,.04},{4,1.04},{6,2.04},{10,3.04},{12,3.04}};relief.stairs.push_back(stair);
    CityReliefPlan::RaisedBlock deck;deck.block=1;deck.level=3;deck.foundation=-1;
    deck.boundary={{-5,0,9},{-1,0,9},{-1,0,14},{-5,0,14}};relief.platforms.push_back(deck);
    relief.stairLandings.push_back({{{1,0,-1},{4,0,-1},{4,0,2},{1,0,2}},.04,-1});
    SettlementFabric fabric;fabric.terrain=relief.openGround;CityModelSettings options;options.groundOnly=true;options.groundVertexSpacing=4;
    CityModelPlan mesh;bool built=planned && buildCityModel(base.circulation,fabric,options,mesh,&why,&relief);
    const auto hasFence=[&](double x,double z,double floor) {
      for(size_t i=0;i<mesh.mesh.triangles.size();++i)if(mesh.materials[i]==55) {
        double x0=1e9,x1=-1e9,z0=1e9,z1=-1e9,y0=1e9,y1=-1e9;
        for(auto index:mesh.mesh.triangles[i]) {auto p=mesh.mesh.vertices[index];x0=std::min(x0,p.x);x1=std::max(x1,p.x);z0=std::min(z0,p.z);z1=std::max(z1,p.z);y0=std::min(y0,p.y);y1=std::max(y1,p.y);}
        if(x0<=x+.08 && x1>=x-.08 && z0<=z+.08 && z1>=z-.08 && y1>floor+.1 && y0<floor+2)return true;
      }
      return false;
    };
    check(built && !hasFence(-1,11,3.04),"stair upper landing opens sideways onto its level deck without a rail blocking the exit");
    check(built && !hasFence(1,1,.04),"stair lower landing opens sideways onto its turning court");
    check(built && hasFence(1,11,3.04) && hasFence(-1,5,2.04),"stair railings remain along exposed drops opposite the exits");
  }
  for(uint64_t seed:{1,17,41,103}) {
    CityBaseSettings s;s.layout=CityBaseLayout::Voronoi;s.seed=seed;s.tiers=4;
    CityBasePlan base,repeat;std::string why;
    bool ok=planCityBase(s,base,&why) && planCityBase(s,repeat,&why),nearest=ok,oblique=false,contained=ok,deterministic=ok;
    if(!ok)std::cerr<<why<<'\n';
    if(ok)for(size_t i=0;i<base.landCells.size();++i) {
      auto site=base.landSites[i];
      deterministic &= base.landCells[i].size()==repeat.landCells[i].size();
      for(size_t j=0;j<base.landCells[i].size();++j) {
        auto p=base.landCells[i][j],q=base.landCells[i][(j+1)%base.landCells[i].size()];
        oblique |= std::abs(p.x-q.x)>.1 && std::abs(p.z-q.z)>.1;
        if(deterministic)deterministic &= std::abs(p.x-repeat.landCells[i][j].x)<1e-9 && std::abs(p.z-repeat.landCells[i][j].z)<1e-9;
        auto distance=[&](Position v){return (p.x-v.x)*(p.x-v.x)+(p.z-v.z)*(p.z-v.z);};
        for(auto other:base.landSites)nearest &= distance(site)<=distance(other)+1e-6;
      }
    }
    if(ok)for(auto e:base.buildable)for(auto p:e.boundary)contained &= inside(base.relief.platforms[e.parent-100].boundary,p);
    check(ok && nearest && oblique,"Voronoi terraces have genuine nearest-site cells and oblique cliff faces");
    check(contained && deterministic,"Voronoi cells preserve buildable cores and deterministic boundaries");

  }
  for(double width:{60.,240.})for(double depth:{34.,60.})for(unsigned tiers:{2,8}) {
    CityBaseSettings s;s.layout=CityBaseLayout::Voronoi;s.width=width;s.tierDepth=depth;s.tiers=tiers;
    CityBasePlan base;CityCompletionPlan city;CityReliefPlan relief;std::string why;
    bool ok=planCityBase(s,base,&why) && populateCityBase(base,city,relief,&why);
    if(ok)for(const auto &e:base.buildable)for(auto p:e.boundary)ok &= inside(base.relief.platforms[e.parent-100].boundary,p);
    if(!ok)std::cerr<<why<<'\n';
    check(ok,"Voronoi dimensional extremes retain usable building envelopes and stair access");
  }
  for(uint64_t seed:{1,17,41,103})for(double width:{60.,84.,120.,240.}) {
    CityBaseSettings settings;settings.layout=CityBaseLayout::Districts;settings.seed=seed;settings.width=width;settings.tiers=3;
    CityBasePlan base,repeat;CityCompletionPlan city;CityReliefPlan relief;std::string why;
    bool ok=planCityBase(settings,base,&why) && populateCityBase(base,city,relief,&why);
    if(!ok)std::cerr<<why<<'\n';
    check(ok && !city.lots.empty(),"separate raised districts support occupied lots across seeds and city widths");
    bool contained=ok,distinct=ok,staggered=false;
    if(ok) {
      for(const auto &e:base.buildable)for(auto p:e.boundary)contained &= inside(base.relief.platforms[e.parent-100].boundary,p);
      const auto &a=base.relief.platforms[0],&b=base.relief.platforms[1];
      distinct &= std::abs(a.level-b.level)>1.;
      double az=0,bz=0;for(auto p:a.boundary)az=std::max(az,p.z);for(auto p:b.boundary)bz=std::max(bz,p.z);
      staggered=std::abs(az-bz)>.01;
      CityReliefPlan recheck;recheck.ground=recheck.openGround=base.relief.openGround;
      contained &= planRaisedCityBlocks(city.skeleton,base.relief.platforms,{},recheck,&why);
    }
    check(contained && distinct && staggered,"districts vary height laterally, offset their seams and preserve populated stair clearance");
    bool deterministic=ok && planCityBase(settings,repeat,&why) && repeat.buildable.size()==base.buildable.size();
    if(deterministic)for(size_t i=0;i<base.relief.platforms.size();++i) {
      auto &a=base.relief.platforms[i],&b=repeat.relief.platforms[i];deterministic &= a.level==b.level && a.boundary.size()==b.boundary.size();
      for(size_t j=0;deterministic && j<a.boundary.size();++j)deterministic &= a.boundary[j].x==b.boundary[j].x && a.boundary[j].z==b.boundary[j].z;
    }
    check(deterministic,"district footprints and elevations are deterministic");
  }
  {
    CityCompletionPlan city;city.revision=city.skeleton.revision=1;
    const auto rect=[](double x,double z,double w,double d){return std::vector<Position>{{x,0,z},{x+w,0,z},{x+w,0,z+d},{x,0,z+d}};};
    for(unsigned b=0;b<2;++b) {
      CitySkeletonRegion block;block.id=100+b;block.kind=CityRegionKind::Block;block.boundary=rect(b*60,0,40,40);city.skeleton.regions.push_back(block);
      for(unsigned k=0;k<4;++k) {
        CitySkeletonRegion lot;lot.id=1000+b*10+k;lot.parent=block.id;lot.kind=CityRegionKind::Lot;
        lot.boundary=rect(b*60+2+(k%2)*25,3+(k/2)*20,10,12);city.skeleton.regions.push_back(lot);
        CityLotDevelopment development;development.lot=lot.id;city.lots.push_back(development);
      }
      CitySmoothedRoute road;road.id=b+1;road.path={{double(b*60),0,-2},{double(b*60+40),0,-2}};road.halfWidth=1.5;city.routes.push_back(road);
    }
    CitySkeletonRegion obstruction;obstruction.id=9999;obstruction.parent=100;obstruction.kind=CityRegionKind::Lot;
    obstruction.boundary=rect(16,1,8,15);city.skeleton.regions.push_back(obstruction);
    CityLotDevelopment obstructingHouse;obstructingHouse.lot=9999;city.lots.push_back(obstructingHouse);
    CityRegionContact contact;contact.a=9999;contact.b=1000;city.skeleton.regionContacts.push_back(contact);
    auto before=city;CityReliefPlan relief;relief.ground=relief.openGround=[](double x,double z){return x<50?.07*z+.9*std::sin(z*3.141592653589793/10):0.;};
    upgradeCityDistricts(city,relief,{});
    check(relief.platforms.size()==1 && relief.platforms[0].block==100,"full-city districts use heightmap relief and leave flat blocks unchanged");
    check(!relief.platforms.empty() && std::abs(relief.platforms[0].stairHalfWidth-1.4)<1e-6,"stair flights fill the complete 2.8 metre retaining-wall slot");
    check(city.lots.size()+1==before.lots.size() && city.routes.size()==before.routes.size() && city.skeleton.regionContacts.empty(),"full-city upgrade reserves stair lots atomically and preserves surrounding streets");
    SettlementFabric fabric;fabric.terrain=relief.openGround;CityModelSettings options;options.groundOnly=true;options.groundVertexSpacing=4;
    CityModelPlan mesh;std::string why;
    bool meshed=buildCityModel(city,fabric,options,mesh,&why,&relief);if(!meshed)std::cerr<<why<<'\n';
    check(meshed,"full-city upgraded relief builds through the shared native mesh path");
    CityReliefPlan repeat;repeat.ground=repeat.openGround=relief.openGround;upgradeCityDistricts(before,repeat,{});
    check(!repeat.platforms.empty() && !relief.platforms.empty() && repeat.platforms.size()==relief.platforms.size() && repeat.platforms.front().level==relief.platforms.front().level,"full-city terrain-driven district selection is deterministic");
    auto gentle=before;CityReliefPlan slopes;slopes.ground=slopes.openGround=[](double,double z){return .01*z;};
    upgradeCityDistricts(gentle,slopes,{});
    check(slopes.platforms.empty(),"gentle hills retain inclined terrain instead of artificial retaining platforms");
    check(slopes.landBlocks.size()==2 && slopes.landBlocks[0].lots.size()==4,
      "one land block owns its city-base slope and dwelling lots");
    CityReliefPlan hill;hill.openGround=hill.ground=[](double x,double z){return .07*z+.08*std::sin(x*.2)*std::sin(z*.2);};
    auto hillCity=gentle;auto source=hill.openGround;upgradeCityDistricts(hillCity,hill,{});
    check(hill.platforms.empty() && hillCity.lots.size()==gentle.lots.size(),"long gentle hills preserve density instead of becoming flat raised districts");
    bool boundary=true,grade=true,changed=false;
    for(double x=0;x<=40;x+=1)for(double z=0;z<=40;z+=1) {
      auto h=hill.openGround;
      if(x==0 || x==40 || z==0 || z==40)boundary &= std::abs(h(x,z)-source(x,z))<1e-9;
      grade &= std::hypot((h(x+.1,z)-h(x-.1,z))/.2,(h(x,z+.1)-h(x,z-.1))/.2)<=.16;
      changed |= std::abs(h(x,z)-source(x,z))>.005;
    }
    check(boundary && grade && changed,"block-wide hill fit smooths its interior and preserves street-edge heights and usable grades");
    double height=hill.openGround(15,15);upgradeCityDistricts(hillCity,hill,{});
    check(hill.openGround(15,15)==height && hill.landBlocks.size()==2,"replanning does not stack hill modifiers");
    check(!relief.landBlocks.empty() && relief.landBlocks[0].kind==CityReliefPlan::LandBlock::Kind::Retained &&
      relief.landBlocks[0].lots.size()==4 && relief.landBlocks[0].accessPath.size()==2,
      "retained block commits lots and access under the same owner");
  }
  {
    using namespace procedural::world::generator;
    auto tasks=settlementGeneratorTasks(1,std::make_shared<Ledger>());
    GenerationSnapshot inputs{{{"world.site-brief",1},{WorldSiteGenerationBrief{},1}},
      {{"settlement.fabric-settings",1},{SettlementFabricSettings{},1}},
      {{"settlement.runtime-settings",1},{SettlementRuntimeSettings{},1}}};
    GenerationPlan schedule;std::string why;bool ok=planGenerators(tasks,inputs,schedule,&why);
    size_t completion=999,relief=0,model=0;
    if(ok)for(size_t i=0;i<schedule.order.size();++i) {
      auto id=tasks[schedule.order[i]].descriptor.id;
      if(id=="city.complete")completion=i;if(id=="city.relief")relief=i;if(id=="city.model")model=i;
    }
    check(ok && completion<relief && relief<model,"full generator publishes reserved lots and planned relief before native meshing");
  }
  {
    CityCompletionPlan city;city.revision=1;
    CitySmoothedRoute a;a.id=1;a.halfWidth=3;a.path={{-14,0,0},{14,0,0}};
    auto b=a;b.id=2;b.path={{0,0,-14},{0,0,14}};city.routes={a,b};
    SettlementFabric fabric;CityModelPlan model;CityModelSettings settings;settings.groundOnly=true;settings.streetPavementMetres=.75;
    std::string error;const bool ok=buildCityModel(city,fabric,settings,model,&error);
    bool recessed=false,seam=false,raised=false;
    for(const auto &p:model.surfaces.pieces){if(p.kind==CitySurfaceKind::Road && p.height<0)recessed=true;if(p.kind==CitySurfaceKind::Sidewalk && p.height>=settings.streetCarveMetres)raised=true;}
    for(size_t i=0;i<model.mesh.triangles.size();++i){auto t=model.mesh.triangles[i];auto p=model.mesh.vertices[t[0]],q=model.mesh.vertices[t[1]],r=model.mesh.vertices[t[2]];double x=(p.x+q.x+r.x)/3,z=(p.z+q.z+r.z)/3;
      if(std::hypot(x,z)<1 && std::abs(model.mesh.expectedNormals[i].y)<.1)seam=true;}
    check(ok && !recessed && raised,"urban paving rises above flush streets without excavating the ground");
    check(ok && !seam,"connected street junction has no curb across its ownership seams");
    bool solidPaving=true;size_t roadFaces=0;
    for(size_t i=0;i<model.mesh.triangles.size();++i)if(model.materials[i]==50 && std::abs(model.mesh.expectedNormals[i].y)>.9) {
      ++roadFaces;solidPaving &= model.surfaceMaterials[i]==13 || model.surfaceMaterials[i]==32;
      for(const auto &corner:model.groundWeights[i])for(double weight:corner)solidPaving &= weight==0.;
    }
    check(roadFaces>0 && solidPaving,"road carriageways use explicit paving without a second terrain-mask road inside them");
  }
  for(double slope:{.08,.14}) {
    CityCompletionPlan city;city.revision=city.skeleton.revision=17;
    CitySkeletonRegion block;block.id=100;block.kind=CityRegionKind::Block;block.landUse=CityLandUse::UrbanBlock;
    block.boundary={{0,0,0},{48,0,0},{48,0,22},{0,0,22}};city.skeleton.regions.push_back(block);
    for(unsigned k=0;k<4;++k) {
      CitySkeletonRegion lot;lot.id=1000+k;lot.parent=100;lot.kind=CityRegionKind::Lot;lot.landUse=CityLandUse::UrbanLot;
      double x=k*12.;lot.boundary={{x+.2,0,1},{x+11.8,0,1},{x+11.8,0,21},{x+.2,0,21}};lot.area=232;
      city.skeleton.regions.push_back(lot);CityLotDevelopment d;d.lot=lot.id;d.rowHouse=true;d.minimumFloors=d.maximumFloors=2;d.desiredRooms=4;d.preferredFootprintMetres2=150;city.lots.push_back(d);
    }
    CitySmoothedRoute route;route.id=1;route.path={{0,0,-1},{48,0,-1}};route.halfWidth=1;city.routes.push_back(route);
    SettlementFabric fabric;fabric.terrain=[slope](double,double z){return slope*z;};
    CityModelSettings options;options.groundVertexSpacing=4;options.warpToLot=false;
    CityModelPlan model;std::string why;bool built=buildCityModel(city,fabric,options,model,&why);
    if(!built)std::cerr<<why<<'\n';
    check(built && model.buildings.size()==4,"sloping frontage reserves entry space without losing its four dwelling lots");
    bool level=built && !model.buildings.empty();
    for(const auto &building:model.buildings)for(auto p:building.footprint)level &= std::abs(p.y-building.footprint.front().y)<1e-6;
    check(level,"houses on inclined ground retain horizontal dwelling footprints");
  }
  {
    CityLandformPlan land;land.facetPeakBoost=1.;
    for(unsigned i=0;i<2;++i) {
      CityLandformCell c;c.id=i+1;c.centre={i*100.+50,0,50};
      c.boundary={{i*100.,0,0},{i*100.+100,0,0},{i*100.+100,0,100},{i*100.,0,100}};
      land.cells.push_back(c);
    }
    auto plane=cityFacetTerrain(land,[](double x,double z){return .1*x+.2*z;});
    check(plane.faces.size()==4 && std::abs(plane.height(37,61)-15.9)<1e-8,
          "Voronoi terrain keeps planar cells at two faces without a sampling grid");
    auto hill=cityFacetTerrain(land,[](double x,double z){return 12*std::max(0.,1-std::hypot(x-50,z-50)/65);});
    check(hill.faces.size()==12 && std::abs(hill.height(50,50)-12)<1e-8,
          "Voronoi terrain adds a crest only where a cell needs one");
    check(std::abs(hill.height(52,50)-hill.height(50,50))<1e-8,
          "styled terrain has a broad polygon crown before triangulation");
    auto angularLand=land;angularLand.facetCrownFraction=0;
    auto angular=cityFacetTerrain(angularLand,[](double x,double z){return 12*std::max(0.,1-std::hypot(x-50,z-50)/65);});
    check(angular.faces.size()==6 && angular.height(52,50)<angular.height(50,50),
          "zero crown fraction selects sparse angular hills");
    auto boldLand=land;boldLand.facetPeakBoost=1.6;
    auto bold=cityFacetTerrain(boldLand,[](double x,double z){return 12*std::max(0.,1-std::hypot(x-50,z-50)/65);});
    check(bold.height(50,50)>hill.height(50,50) && std::abs(bold.height(100,40)-hill.height(100,40))<1e-8,
          "stronger terrain silhouettes preserve shared cell boundary heights");
    check(std::abs(hill.height(100-1e-7,40)-hill.height(100+1e-7,40))<1e-6,
          "adjacent Voronoi terrain cells share continuous edge heights");
    CityCompletionPlan city;city.revision=city.skeleton.revision=1;
    CitySkeletonRegion block;block.id=1;block.kind=CityRegionKind::Block;
    block.boundary={{0,0,0},{200,0,0},{200,0,100},{0,0,100}};city.skeleton.regions.push_back(block);
    CitySmoothedRoute road;road.id=2;road.path={{0,0,30},{200,0,30}};road.halfWidth=2;city.routes.push_back(road);
    SettlementFabric fabric;fabric.landforms=land;fabric.terrain=hill.height;
    CityModelSettings settings;settings.groundOnly=true;settings.streetCarveMetres=0;settings.textureStreets=false;
    CityModelPlan model;std::string why;
    const bool built=buildCityModel(city,fabric,settings,model,&why);
    if(!built)std::cerr<<why<<'\n';
    std::cout<<"Contour road fixture triangles: "<<model.mesh.triangles.size()<<" roads: "<<model.roadTriangles<<" built: "<<built<<"\n";
    check(built && model.roadTriangles>0 && model.mesh.triangles.size()<1000,
          "road polygons constrain the coarse Voronoi cover before CDT");
    settings.textureStreets=true;CityModelPlan painted;
    check(buildCityModel(city,fabric,settings,painted,&why) && painted.roadTriangles==0 && !painted.surfaces.influences.empty(),
          "texture streets retain paint regions without separate road geometry");

  }
  {
    CityCompletionPlan city;city.historicalLots={{1,101},{2,102}};
    for(unsigned k=0;k<2;++k) {
      CityLotDevelopment lot;lot.lot=101+k;city.lots.push_back(lot);
      CitySkeletonRegion region;region.id=lot.lot;region.boundary={{k*20.,0,1},{k*20.+1,0,1},{k*20.+1,0,2}};
      city.skeleton.regions.push_back(region);
    }
    const std::vector<std::vector<Position>> paths={{{0,0,0},{10,0,0}},{{10,0,0},{20,0,0}},{{10,0,0},{10,0,-10}},{{50,0,0},{60,0,0}}};
    for(size_t k=0;k<paths.size();++k){CitySmoothedRoute r;r.id=k+1;r.path=paths[k];r.halfWidth=1;city.routes.push_back(r);}
    check(pruneUnoccupiedCityStreets(city,{})==2 && city.routes.size()==2 && city.routes[0].id==1 && city.routes[1].id==2,
          "occupancy pruning removes empty spurs and islands while connecting inhabited frontages");
    check(pruneUnoccupiedCityStreets(city,{})==0,"occupancy street pruning is stable on repeated generation");
  }
  {
    const auto rect=[](double x,double z,double w,double h){procedural::geom2::Polygon p;for(auto v:std::vector<Position>{{x,0,z},{x+w,0,z},{x+w,0,z+h},{x,0,z+h}})p.outer.push_back({procedural::geom2::toUnit(v.x),procedural::geom2::toUnit(v.z)});return p;};
    CitySurfaceStructure paint;paint.influences.push_back({rect(0,0,20,2),2,1,0,true});
    paint.influences.push_back({rect(0,2.8,20,2),2,1,0,true});
    paint.influences.push_back({rect(0,10,20,2),2,1,0,true});
    std::string why;
    check(mergeCityRoadPaint(paint,{},1.5,&why) && paint.influences.size()==2,
          "nearby road paint merges while genuinely separate streets remain separate");
    CitySurfaceStructure blocked;blocked.influences.push_back({rect(0,0,20,2),2,1,0,true});
    blocked.influences.push_back({rect(0,2.8,20,2),2,1,0,true});
    check(mergeCityRoadPaint(blocked,{rect(-1,2,22,.8)},1.5,&why) && blocked.influences.size()==2,
          "street merging preserves a protected lot between close routes");
    CitySurfaceStructure sections;sections.influences.push_back({rect(50,0,40,2),2,1,0,true});
    sections.influences.push_back({rect(50,2.8,40,2),2,1,0,true});auto whole=sections;
    bool same=mergeCityRoadPaint(sections,{},1.5,&why,true) && mergeCityRoadPaint(whole,{},1.5,&why);
    double sectionArea=0,wholeArea=0;
    for(const auto &p:sections.influences)sectionArea+=procedural::geom2::areaMetres(p.polygon);
    for(const auto &p:whole.influences)wholeArea+=procedural::geom2::areaMetres(p.polygon);
    check(same && std::abs(sectionArea-wholeArea)<.01,"sectioned street merges preserve coverage across section boundaries");
  }
  {
    std::vector<std::array<Position,3>> planes={{{{0,0,0},{10,10,0},{10,10,10}}},{{{0,0,0},{10,10,10},{0,0,10}}}};
    auto contours=planCityElevationContours(planes,2.);double area=0;bool level=true;
    for(const auto &line:contours.lines)level &= std::abs(line[0].y-line[1].y)<1e-9 && std::abs(line[0].x-line[0].y)<1e-9;
    for(const auto &band:contours.bands){double a=0;for(size_t k=0;k<band.size();++k)a+=band[k].x*band[(k+1)%band.size()].z-band[k].z*band[(k+1)%band.size()].x;area+=std::abs(a)*.5;}
    check(level && !contours.lines.empty() && std::abs(area-100)<1e-8,
          "elevation contours precede triangulation and partition slopes without gaps or overlaps");
    auto flat=planCityElevationContours({{{{0,4,0},{10,4,0},{0,4,10}}}},2.);
    check(flat.bands.size()==1 && flat.lines.empty(),"flat terrain adds no redundant elevation contours");
  }
  {
    CityLandformPlan land;
    for(unsigned z=0;z<3;++z)for(unsigned x=0;x<3;++x) {
      CityLandformCell c;c.id=1+z*3+x;c.centre={x*40.+20,0,z*40.+20};
      c.boundary={{x*40.,0,z*40.},{x*40.+40,0,z*40.},{x*40.+40,0,z*40.+40},{x*40.,0,z*40.+40}};land.cells.push_back(c);
    }
    procedural::geom2::Polygon square;square.outer={{0,0},{100000000,0},{100000000,100000000},{0,100000000}};
    std::vector<procedural::geom2::Polygon> curved{square};std::string smoothingError;
    check(smoothCityContourPolygons(curved,&smoothingError) && curved.size()==1 && curved.front().outer.size()>4 && procedural::geom2::polygonFault(curved.front()).empty(),
          "organic contour smoothing adds outline detail while preserving valid polygon topology");
    auto plan=cityContourTerrain(land,[](double x,double z){return std::max(0.,10.-std::hypot(x-60,z-60)*.15);});
    bool level=!plan.contours.empty() && plan.error.empty() && !plan.faces.empty();
    for(const auto &ring:plan.contours)for(auto p:ring)level &= p.y==ring.front().y && std::abs(plan.height(p.x,p.z)-p.y)<1e-5;
    check(level && plan.contours.size()>2,"connected contour loops own exact elevations before CDT and retain them afterward");
  }
  CityBasePlan sentinel;sentinel.settings.seed=999;CityBaseSettings bad;bad.width=12;
  check(!planCityBase(bad,sentinel) && sentinel.settings.seed==999,"invalid base dimensions fail transactionally");
  return failures?1:0;
}
