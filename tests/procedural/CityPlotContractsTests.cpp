#include "procedural/world/WorldCityPlotContractsIntegration.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include "procedural/world/WorldCityStructuresIntegration.hpp"
#include <iostream>
using namespace procedural::world::rewrite;
static std::vector<Position> rect(double x,double z,double w,double d){return {{x,0,z},{x+w,0,z},{x+w,0,z+d},{x,0,z+d}};}
int main() {
  int failures=0;auto check=[&](bool ok,const char *s){std::cout<<(ok?"PASS ":"FAIL ")<<s<<'\n';failures+=!ok;};
  CityTerrainRouteCandidate candidate;candidate.id=1;candidate.roads.revision=4;
  candidate.roads.roadNodes={{1,CityRoadNodeKind::Endpoint,{0,0,0},{}},{2,CityRoadNodeKind::Endpoint,{60,0,0},{}}};
  CityRoadEdge edge;edge.id=1;edge.from=1;edge.to=2;edge.path={{0,0,0},{60,0,0}};edge.halfWidth=1.5;candidate.roads.roadEdges={edge};
  candidate.benches={{1,1,{CityConstructionKind::Complex,100},rect(2,4,54,12),0}};
  CityTerrainRoutePlan terrain;CityTerrainRouteSettings ts;std::vector<CityTerrainRouteRejection> rejected;std::string why;
  auto flat=[](double,double){return 0.;};
  check(planCityTerrainRoutes({candidate},flat,ts,{}, {},terrain,rejected,&why),"terrain bench fixture solves");
  CityPlotSettings settings;settings.maximumAttempts=128;
  std::vector<CityPlotFrontage> frontage={{100,1,rect(2,4,54,12)}};
  std::vector<CityPlotRequest> demand={{10,30,2,1},{11,30,2,1},{12,30,2,1}};
  CityPlotContractPlan plan,report;
  bool planned=planCityPlotContracts(terrain,frontage,demand,{},settings,plan,report,&why);
  if(!planned){std::cerr<<why<<'\n';for(auto u:report.unmet)std::cerr<<u.building<<": "<<u.reason<<'\n';}
  check(planned&&plan.plots.size()==3,"modular frontage fits real dwelling programmes");
  bool contracts=planned;
  for(const auto &p:plan.plots) {
    contracts&=std::abs(p.house.frame.cellMetres-settings.module)<1e-9 && p.doorWidth>=.85;
    contracts&=p.floor>=terrain.ground(p.door.x,p.door.z)+.1 && !p.steps.empty();
    contracts&=city_plot_detail::area(p.envelope)+1e-5>=p.requestedArea;
    for(const auto &q:plan.plots)if(p.building!=q.building)contracts&=!city_plot_detail::overlap(p.lot,q.lot)&&!city_plot_detail::overlap(p.approach,q.lot);
  }
  check(contracts,"whole modules, real doorways, horizontal floors and clear approaches agree");
  auto original=plan;std::reverse(demand.begin(),demand.end());
  CityPlotContractPlan repeat;
  check(planCityPlotContracts(terrain,frontage,demand,{},settings,repeat,report,&why)&&!repeat.plots.empty()&&repeat.plots.front().building==plan.plots.front().building,
        "history input order does not affect deterministic allocation");
  auto narrow=frontage;narrow[0].bench=rect(2,4,2,2);
  check(!planCityPlotContracts(terrain,narrow,demand,{},settings,plan,report,&why)&&report.unmet.size()==3&&plan.revision==original.revision,
        "capacity fails before meshing without replacing accepted contracts");
  std::vector<CityTerrainReservation> blocked={{1,rect(-1,1.5,65,2.5)}};
  check(!planCityPlotContracts(terrain,frontage,demand,blocked,settings,repeat,report,&why),"reserved corridor blocks doorway approaches");
  auto steep=terrain;steep.ground=[](double x,double){return x;};
  check(!planCityPlotContracts(steep,frontage,demand,{},settings,repeat,report,&why),"nonhorizontal lots require terrain replanning instead of buried floors");
  auto bad=demand;bad.push_back(bad.front());
  check(!planCityPlotContracts(terrain,frontage,bad,{},settings,repeat,report,&why),"duplicate historical identities rejected");
  auto tiny=settings;tiny.module=.4;
  check(!planCityPlotContracts(terrain,frontage,demand,{},tiny,repeat,report,&why),"modules cannot be squeezed to rescue capacity");
  std::vector<CityPlotRequest> ten;
  for(uint64_t id=20;id<29;++id)ten.push_back({id,30,2,1});
  ten.push_back({99,100000,2,1});auto larger=settings;larger.maximumAttempts=1024;
  CityPlotContractPlan ninety;
  check(planCityPlotContracts(terrain,frontage,ten,{},larger,ninety,report,&why)&&ninety.plots.size()==9&&
        ninety.unmet.size()==1&&ninety.unmet[0].requiredArea==100000,
        "ninety percent gate retains the remaining historical request and area");
  auto strict=larger;strict.minimumPlacement=1;
  check(!planCityPlotContracts(terrain,frontage,ten,{},strict,repeat,report,&why),
        "full placement policy rejects the same insufficient capacity");
  CityCompletionPlan city;city.revision=4;city.skeleton=candidate.roads;
  CitySkeletonRegion block;block.id=100;block.kind=CityRegionKind::Block;block.landUse=CityLandUse::UrbanBlock;block.boundary=rect(-2,-4,65,23);city.skeleton.regions={block};
  CityReliefPlan relief;
  bool applied=planned&&applyCityPlotContracts(original,city,relief,&why);
  check(applied&&city.historicalLots.size()==3,"accepted contracts create historical lots in normal city pipeline");
  if(applied) {
    SettlementFabric fabric;fabric.terrain=flat;CityModelSettings options;options.streetDetails=false;
    CityModelPlan model;bool built=buildCityModel(city,fabric,options,model,&why,&relief);
    if(!built)std::cerr<<why<<'\n';
    check(built&&model.buildings.size()==original.plots.size(),"normal city mesh emits every contracted dwelling");
    const auto &first=original.plots.front();
    CityStructureProgramme structures;
    CityStructureBox support{first.lot[0].x,first.floor-.5,first.lot[0].z,first.lot[2].x,first.floor,first.lot[2].z};
    structures.platforms.push_back({900,first.building,support,{}});
    CityStructurePlan assembly;
    bool composed=planCityStructures(city.plotContracts,structures,assembly,&why)&&applyCityStructures(assembly,city,&why);
    if(!composed)std::cerr<<why<<'\n';
    check(composed&&assembly.supportedBuildings.count(first.building),"platform owns support of real contracted dwelling");
    CityModelPlan supportedModel;
    check(composed&&buildCityModel(city,fabric,options,supportedModel,&why,&relief)&&supportedModel.buildings.size()==model.buildings.size(),
          "real dwellings and shared structural support emit together");
    bool same=built;
    for(const auto &c:original.plots) {
      auto b=std::find_if(model.buildings.begin(),model.buildings.end(),[&](const auto &b){return b.id==c.building;});
      same&=b!=model.buildings.end();
      if(b!=model.buildings.end()) {
        same&=std::abs(b->base-c.floor)<1e-7;
        bool door=false;for(auto p:b->exteriorDoors)door|=std::hypot(p.x-c.door.x,p.z-c.door.z)<1e-6&&std::abs(p.y-c.door.y)<.02;
        same&=door;
      }
    }
    check(same,"mesher preserves agreed floor levels and actual door positions");
    auto stale=relief;stale.constructionTerrain=std::make_shared<const CityTerrainRoutePlan>(terrain);
    auto previous=model.buildings.size();
    check(!buildCityModel(city,fabric,options,model,&why,&stale)&&model.buildings.size()==previous,
          "mesher rejects a different terrain snapshot transactionally");
  }
  std::cout<<"CITY_PLOT_CONTRACTS_TESTS failures="<<failures<<'\n';return failures?1:0;
}
