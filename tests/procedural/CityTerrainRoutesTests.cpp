#include "procedural/world/WorldCityTerrainRoutesMesh.hpp"
#include "procedural/world/WorldCityTerrainRoutesIntegration.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include <iostream>
#include <fstream>
using namespace procedural::world::rewrite;
static std::vector<Position> rect(double x,double z,double w,double d){return {{x,0,z},{x+w,0,z},{x+w,0,z+d},{x,0,z+d}};}
static CityTerrainRouteCandidate candidate() {
  CityTerrainRouteCandidate c;c.id=1;c.roads.revision=3;
  c.roads.roadNodes={{1,CityRoadNodeKind::Endpoint,{0,0,0},{}},{2,CityRoadNodeKind::Endpoint,{40,0,0},{}}};
  CityRoadEdge e;e.id=10;e.from=1;e.to=2;e.path={{0,0,0},{40,0,0}};e.halfWidth=1;c.roads.roadEdges={e};return c;
}
int main(int argc,char **argv) {
  int failures=0;auto check=[&](bool v,const char *s){std::cout<<(v?"PASS ":"FAIL ")<<s<<'\n';failures+=!v;};
  const auto flat=[](double,double){return 0.;};
  const auto valley=[](double x,double z){return -5*std::max(0.,1-std::hypot(x-20,z)/5.);};
  auto base=candidate();CityTerrainRouteSettings settings;settings.maximumCut=1;settings.maximumFill=.5;
  CityTerrainRoutePlan plan;std::vector<CityTerrainRouteRejection> rejected;std::string why;
  auto options=cityRouteDetours(base,10,{12});
  check(planCityTerrainRoutes(options,valley,settings,{}, {},plan,rejected,&why),"route search finds a bounded-earthwork detour");
  check(plan.candidate!=1 && !rejected.empty() && rejected[0].candidate==1,"failed direct route is retried rather than deeply carved");
  check(plan.maximumCut<=1 && plan.maximumFill<=.5 && plan.profiles.streets[0].maximumGrade<=settings.streets.maximumStreetGrade+1e-7,"accepted route obeys sampled cut/fill and grade budgets");
  auto inclined=cityRouteDetours(candidate(),10,{12});inclined.erase(inclined.begin());
  check(planCityTerrainRoutes(inclined,[](double x,double){return .02*x;},settings,{}, {},plan,rejected,&why),
        "mitered graded turns remain continuous without overlapping road ribbons");
  check(planCityTerrainRoutes(options,valley,settings,{}, {},plan,rejected,&why),"valley solve remains independent of prior candidate");
  auto chosen=plan.candidate;std::reverse(options.begin(),options.end());
  check(planCityTerrainRoutes(options,valley,settings,{}, {},plan,rejected,&why)&&plan.candidate==chosen,"candidate ordering does not change selected plan");
  auto saved=plan;settings.maximumCut=.1;
  check(!planCityTerrainRoutes(options,valley,settings,{}, {},plan,rejected,&why)&&plan.candidate==saved.candidate,"exhausted budgets preserve previously accepted plan");
  settings.maximumCut=1;
  auto bad=base;bad.id=2;bad.roads.roadEdges.clear();
  check(planCityTerrainRoutes({base,bad},flat,settings,{}, {},plan,rejected,&why)&&rejected.size()==1,"candidate cannot satisfy budgets by deleting a required street");
  bad=base;bad.id=2;bad.roads.roadNodes[1].position.x=41;
  check(planCityTerrainRoutes({base,bad},flat,settings,{}, {},plan,rejected,&why)&&rejected.size()==1,"candidate cannot move a required anchor");
  base.benches={{20,10,{CityConstructionKind::Plot,20},rect(10,4,8,5),.2}};
  std::vector<std::array<Position,2>> contours={{{{0,0,6},{30,0,6}}}};
  check(planCityTerrainRoutes({base},flat,settings,{},contours,plan,rejected,&why),"road and horizontal building bench solve together");
  if(!why.empty())std::cerr<<why<<'\n';
  check(std::abs(plan.ground(14,6)+.135)<1e-7 && std::abs(plan.ground(14,0)+.335)<1e-7,"bench uses serving street elevation and published ground replaces terrain");
  check(plan.ground(14,30)==flat(14,30),"unedited distant terrain is preserved");
  bool removed=true,shared=true;
  std::map<std::pair<int64_t,int64_t>,double> heights;
  for(const auto &line:plan.constraints) {
    auto middle=city_terrain_detail::lerp(line[0],line[1],.5);
    if(std::abs(middle.z-6)<1e-8 && middle.x>10+1e-6&&middle.x<18-1e-6)removed=false;
    for(auto v:line) {
      auto key=std::make_pair(llround(v.x*1e6),llround(v.z*1e6));auto [it,added]=heights.emplace(key,v.y);
      shared&=added||std::abs(it->second-v.y)<1e-10;
    }
  }
  check(removed&&shared,"old contours are clipped inside edits and shared XY constraints agree");
  auto valid=plan;
  bad=base;bad.benches[0].boundary=rect(10,0,8,5);
  check(!planCityTerrainRoutes({bad},flat,settings,{}, {},plan,rejected,&why)&&rejected[0].reason.find("overlapping")!=std::string::npos,"incompatible overlapping bench and road levels reject before CDT");
  auto fill=base;fill.id=2;fill.benches[0].offset=2;
  check(planCityTerrainRoutes({fill,base},flat,settings,{}, {},plan,rejected,&why)&&plan.candidate==base.id&&!rejected.empty(),"over-budget bench fill retries another joint candidate");
  std::vector<CityTerrainReservation> water={{1,rect(18,-4,4,8)}};
  check(!planCityTerrainRoutes({candidate()},flat,settings,water,{},plan,rejected,&why),"water crossing requires a separate structural design instead of terrain fill");
  check(planCityTerrainRoutes(cityRouteDetours(candidate(),10,{12}),flat,settings,water,{},plan,rejected,&why)&&plan.candidate!=1,"route retry goes around protected channel including bank clearance");
  settings.maximumEarthwork=.01;
  check(!planCityTerrainRoutes({base},flat,settings,{}, {},plan,rejected,&why),"aggregate earthwork budget enforced");settings.maximumEarthwork=100000;
  settings.maximumSamples=2;
  check(!planCityTerrainRoutes({base},flat,settings,{}, {},plan,rejected,&why),"sample work budget is bounded");settings.maximumSamples=250000;
  check(!planCityTerrainRoutes({base},[](double,double){return NAN;},settings,{}, {},plan,rejected,&why),"nonfinite source fails transactionally");
  CityTerrainRouteMesh mesh;
  bool meshed=meshCityTerrainRoutes(valid,-5,-8,45,15,mesh,&why);
  if(!meshed)std::cerr<<why<<'\n';
  check(meshed&&!mesh.triangles.empty(),"edited construction terrain is triangulated through CDT");
  double area=0;bool clean=meshed;
  for(auto t:mesh.triangles) {
    auto a=mesh.vertices[t[0]],b=mesh.vertices[t[1]],c=mesh.vertices[t[2]];
    double face=city_terrain_detail::cross(a,b,c)*.5;area+=face;clean&=face>0;
    Position q{(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3};
    clean&=std::abs(q.y-valid.ground(q.x,q.z))<=.050001;
  }
  check(clean&&std::abs(area-50*23)<1e-5,"one terrain layer covers domain once with bounded sampled mesh error");
  std::map<std::pair<uint32_t,uint32_t>,unsigned> incidence;
  for(auto t:mesh.triangles)for(unsigned i=0;i<3;++i){auto a=t[i],b=t[(i+1)%3];if(a>b)std::swap(a,b);++incidence[{a,b}];}
  bool conforming=meshed;
  for(auto [edge,count]:incidence) {
    auto a=mesh.vertices[edge.first],b=mesh.vertices[edge.second];
    bool border=(a.x==-5&&b.x==-5)||(a.x==45&&b.x==45)||(a.z==-8&&b.z==-8)||(a.z==15&&b.z==15);
    conforming&=count==(border?1u:2u);
  }
  check(conforming,"adaptive terrain shares every internal edge without cracks or T junctions");
  bool bench=true;size_t benchFaces=0;
  for(auto t:mesh.triangles) {
    auto a=mesh.vertices[t[0]],b=mesh.vertices[t[1]],c=mesh.vertices[t[2]];
    Position q{(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3};
    if(q.x>10.001&&q.x<17.999&&q.z>4.001&&q.z<8.999){++benchFaces;bench&=std::abs(q.y+.135)<1e-6;}
  }
  check(bench&&benchFaces>0,"building bench mesh is horizontal without original ground above it");
  CityCompletionPlan city;city.revision=3;city.skeleton=base.roads;CityReliefPlan relief;
  check(applyCityTerrainRoutes(valid,city,relief,&why)&&city.routes.size()==1&&relief.openGround(14,6)==valid.ground(14,6),"accepted terrain routes feed normal city circulation and relief");
  CitySkeletonRegion block;block.id=1;block.kind=CityRegionKind::Block;block.boundary=rect(-5,-8,50,23);city.skeleton.regions={block};
  SettlementFabric fabric;fabric.terrain=flat;CityModelSettings modelSettings;modelSettings.groundOnly=true;modelSettings.polygonTerrain=false;
  CityModelPlan model;
  bool built=buildCityModel(city,fabric,modelSettings,model,&why,&relief);
  if(!built)std::cerr<<why<<'\n';
  check(built&&!model.mesh.triangles.empty(),"normal city mesh builder consumes joint terrain solution");
  city.lots={{20,true}};auto old=city.revision;
  check(!applyCityTerrainRoutes(valid,city,relief,&why)&&city.revision==old,"late rerouting cannot move roads through occupied lots");
  if(argc>1&&meshed) {
    std::ofstream out(argv[1]);for(auto v:mesh.vertices)out<<"v "<<v.x<<' '<<v.y<<' '<<v.z<<'\n';
    for(auto t:mesh.triangles)out<<"f "<<t[0]+1<<' '<<t[1]+1<<' '<<t[2]+1<<'\n';
  }
  std::cout<<"CITY_TERRAIN_ROUTES_TESTS failures="<<failures<<'\n';return failures?1:0;
}
