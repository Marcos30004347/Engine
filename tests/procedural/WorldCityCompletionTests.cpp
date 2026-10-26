#include "procedural/world/WorldCityWaterways.hpp"
#include "procedural/world/WorldCityCompletion.hpp"

#include "procedural/world/WorldCityRelief.hpp"
#include <cmath>
#include <iostream>
#include <set>

using namespace procedural::world::rewrite;

namespace {
std::vector<Position> rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}
}

int main()
{
  int failures = 0;
  auto check = [&](bool condition, const char *message) {
    std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
    failures += !condition;
  };

  {
    CitySkeleton city;city.revision=1;
    city.regions={{10,10,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Merchant,rect(0,10,40,20),false,800},
                  {11,11,10,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Merchant,rect(4,14,10,10),false,100}};
    CityReliefPlan plan;plan.openGround=plan.ground=[](double,double){return 0.;};
    CityReliefPlan::RaisedBlock deck;deck.block=10;deck.boundary=rect(0,10,40,20);deck.level=4.2;deck.stairHalfWidth=1.2;
    deck.accessPath={{2,0,2},{20,2.1,2},{20,2.1,6},{2,4.2,6},{2,4.2,10}};
    CityReliefSettings settings;std::string why;
    check(planRaisedCityBlocks(city,{deck},settings,plan,&why),"raised block accepts a switchback with turning landings");
    check(plan.platforms.size()==1 && plan.stairs.size()==2 && plan.stairLandings.size()==2,"switchback emits separate deck, flights and landing structures");
    check(plan.openGround(6,18)==0 && plan.ground(6,18)==0 && plan.regionLevel[11]==4.2,"raised lots share the deck elevation without modifying base terrain");
    bool valid=true;
    for(const auto &stair:plan.stairs){double d=0,h=stair.a.y+.04;for(auto band:stair.bands){valid&=band.first-d>=settings.stepGoingMetres-1e-6 && band.second-h<=settings.stepRiseMetres+1e-6;d=band.first;h=band.second;}}
    check(valid,"switchback flights preserve minimum going and maximum rise");
    CityReliefPlan empty;empty.openGround=empty.ground=[](double,double){return 0.;};
    auto invalid=deck;invalid.accessPath={{2,0,9},{2,4.2,10}};
    check(!planRaisedCityBlocks(city,{invalid},settings,empty,&why) && empty.platforms.empty(),"short stair request fails atomically instead of squashing treads");
    invalid=deck;invalid.boundary=rect(0,10,5,5);
    check(!planRaisedCityBlocks(city,{invalid},settings,empty,&why),"deck planner rejects lots outside its support footprint");
    auto bridged=deck;bridged.accessPath.clear();
    bridged.boundary={{0,0,10},{20,0,10},{20,0,28},{30,0,28},{30,0,10},{40,0,10},{40,0,30},{0,0,30}};
    bridged.accessFrom={25,0,10};bridged.accessTo={25,4.2,28};
    bridged.bridges={{{20,4.2,13},{30,4.2,13},1.3,1.2,.4}};
    check(!planRaisedCityBlocks(city,{bridged},settings,empty,&why) && empty.platforms.empty(),"bridge across a stair court is rejected even with nominal headroom");
    CityReliefPlan blocked;blocked.openGround=blocked.ground=[](double,double){return 0.;};
    bridged.bridges.front().thickness=3.;
    check(!planRaisedCityBlocks(city,{bridged},settings,blocked,&why) && blocked.platforms.empty(),"thicker stair-court bridge is also rejected atomically");

  }
  {
    CitySkeleton city;city.revision=1;
    city.regions={{1,1,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Merchant,rect(0,10,10,20),false,200},
                  {2,2,0,CityRegionKind::Block,CityLandUse::UrbanBlock,DistrictUse::Merchant,rect(30,10,10,20),false,200}};
    CityReliefPlan::RaisedBlock a,b;a.block=1;b.block=2;a.boundary=city.regions[0].boundary;b.boundary=city.regions[1].boundary;
    a.level=b.level=12;a.stairHalfWidth=b.stairHalfWidth=1.2;
    a.accessFrom={5,0,-40};a.accessTo={5,12,10};b.accessFrom={35,0,-40};b.accessTo={35,12,10};
    a.bridges={{{10,12,22},{30,12,22},1.3,1.2,.4}};
    CityReliefPlan relief;relief.ground=relief.openGround=[](double,double){return 0.;};std::string why;
    check(planRaisedCityBlocks(city,{a,b},{},relief,&why),"bridge can connect separate high platforms without crossing their stairs");
    CityReliefPlan blocked;blocked.ground=blocked.openGround=relief.openGround;
    b.accessFrom={12,0,-30};b.accessTo={30,12,22};
    check(!planRaisedCityBlocks(city,{a,b},{},blocked,&why) && blocked.platforms.empty() && why.find("crosses a staircase")!=std::string::npos,
      "bridge checks include stair routes from later platform requests");
  }
  {
    CitySkeleton roads;roads.revision=1;
    roads.roadNodes={{1,CityRoadNodeKind::Endpoint,{0,0,0},{}},{2,CityRoadNodeKind::Endpoint,{0,0,30},{}}};
    roads.roadEdges={{1,1,1,2,CityRoadKind::Street,{{0,0,0},{0,0,30}},2.}};
    CityReliefSettings settings;settings.stairStreets=true;settings.steppedWayGrade=.12;
    CityReliefPlan relief;std::string why;
    check(planCityRelief(roads,[](double,double z){return z*.2;},settings,relief,&why) && relief.stairs.size()==1,"steep public route receives an explicit staircase");
    if(!relief.stairs.empty()) {
      const auto &stair=relief.stairs.front();double lastDistance=0,lastHeight=stair.a.y+.04;unsigned landings=0;bool safe=true;
      for(auto band:stair.bands) {
        safe&=band.first-lastDistance>=settings.stepGoingMetres-1e-6 && band.second-lastHeight<=settings.stepRiseMetres+1e-6 && band.second>=lastHeight-1e-6;
        if(std::abs(band.second-lastHeight)<1e-6)++landings;
        safe&=relief.openGround(0,(lastDistance+band.first)*.5)<band.second-.1;
        lastDistance=band.first;lastHeight=band.second;
      }
      check(safe && landings>=3,"stair treads retain safe rise/going, intermediate landings, and recessed terrain");
      check(std::abs(lastDistance-30)<1e-6 && std::abs(lastHeight-stair.b.y-.04)<1e-6,"stairs meet both planned street levels exactly");
    }
  }
  {
    CitySkeleton roads;
    roads.revision=1;
    CityGrowthPlan roadGrowth;
    CityGrowthWall distantWall;
    distantWall.id=1;
    distantWall.boundary=rect(0,0,10,10);
    roadGrowth.walls.push_back(distantWall);
    roads.roadNodes={{1,CityRoadNodeKind::Endpoint,{300,0,300},{30}},
                     {2,CityRoadNodeKind::Junction,{320,0,300},{30,11}},
                     {3,CityRoadNodeKind::Junction,{340,0,320},{11,40}},
                     {4,CityRoadNodeKind::Endpoint,{340,0,340},{40}}};
    roads.roadEdges={{30,1,1,2,CityRoadKind::Approach,{{300,0,300},{320,0,300}},3},
                     {11,1,2,3,CityRoadKind::Approach,{{320,0,300},{340,0,320}},3},
                     {40,1,3,4,CityRoadKind::Approach,{{340,0,320},{340,0,340}},3}};
    CityCompletionPlan result;
    std::string why;
    check(completeCityPlan(roads,{},roadGrowth,{},{},result,&why) && result.routes.size()==1 &&
          result.routes.front().edges.size()==3,
          "a middle-edge seed still produces one complete smooth route");
    roads.roadNodes={{1,CityRoadNodeKind::Endpoint,{300,0,300},{11}},
                     {2,CityRoadNodeKind::Endpoint,{340,0,340},{11}}};
    roads.roadEdges={{11,1,1,2,CityRoadKind::Approach,{{300,0,300},{340,0,300},{340,0,340}},3}};
    check(completeCityPlan(roads,{},roadGrowth,{},{},result,&why),"broad bend composes");
    auto localRoads=roads;
    for(auto &edge:localRoads.roadEdges)edge.kind=CityRoadKind::Street;
    CityCompletionPlan localResult;
    bool reservedWidth=completeCityPlan(localRoads,{},roadGrowth,{},{},localResult,&why);
    for(const auto &route:localResult.routes)for(auto id:route.edges)
      for(const auto &edge:roads.roadEdges)if(edge.id==id)reservedWidth &= route.halfWidth+1e-8>=edge.halfWidth;
    check(reservedWidth,"completed streets preserve the width reserved by the block plan");

    if (result.routes.empty()) return 1;
    double deviation=0;
    for (auto p:result.routes.front().path) deviation=std::max(deviation,std::min(std::abs(p.z-300),std::abs(p.x-340)));
    check(deviation>2 && result.routes.front().path.front().x==300 && result.routes.front().path.back().z==340,
          "width-scaled curve rounds metres of the bend while retaining endpoints");
    roads.regions.push_back({80,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Residential,rect(331,304,3,3),false,9});
    check(completeCityPlan(roads,{},roadGrowth,{},{},result,&why),"bend with protected lot composes");
    bool clear=true;
    for (auto p:result.routes.front().path)
    {
      const double dx=std::max({331-p.x,0.,p.x-334}), dz=std::max({304-p.z,0.,p.z-307});
      clear=clear && std::hypot(dx,dz)>=result.routes.front().halfWidth;
    }
    check(clear,"curve backtracking keeps the full road width outside a nearby lot");
    roads.regions.push_back({81,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Residential,rect(306,305,6,6),false,36});
    roads.regions.push_back({82,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Residential,rect(316,305,6,6),false,36});
    roads.roadEdges.front().kind=CityRoadKind::Street;
    roadGrowth.walls.clear();roadGrowth.historical=true;
    check(completeCityPlan(roads,{},roadGrowth,{},{},result,&why) && !result.routes.empty() &&
          std::any_of(result.routes.begin(),result.routes.end(),[](const auto &r){return std::find(r.edges.begin(),r.edges.end(),11)!=r.edges.end() && r.groundCover==CityGroundCover::Stone;}),
          "unwalled urban streets receive planned stone paving");
    roads.roadEdges.front().kind=CityRoadKind::FieldTrack;
    check(completeCityPlan(roads,{},roadGrowth,{},{},result,&why) && !result.routes.empty() &&
          std::any_of(result.routes.begin(),result.routes.end(),[](const auto &r){return std::find(r.edges.begin(),r.edges.end(),11)!=r.edges.end() && r.groundCover!=CityGroundCover::Stone;}),
          "rural tracks are not paved by nearby urban plots");
  }

  CitySkeleton skeleton;
  skeleton.revision = 5;
  skeleton.roadNodes = {{10, CityRoadNodeKind::Endpoint, {-20, 0, 20}, {11}}, {12, CityRoadNodeKind::Junction, {80, 0, 20}, {11, 14}}, {13, CityRoadNodeKind::Endpoint, {140, 0, 40}, {14}}};
  skeleton.roadEdges = {{11, 1, 10, 12, CityRoadKind::Approach, {{-20, 0, 20}, {80, 0, 20}}, 2},
                        {14, 1, 12, 13, CityRoadKind::Approach, {{80, 0, 20}, {110, 0, 50}, {140, 0, 40}}, 2}};
  skeleton.regions.push_back({30, 1, 0, CityRegionKind::District, CityLandUse::Harbour, DistrictUse::Harbor, rect(8, 8, 20, 10), false, 200});
  skeleton.regions.push_back({31,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Merchant,rect(10,10,3,4),false,12});
  skeleton.roadNodes.push_back({101,CityRoadNodeKind::Endpoint,{180,0,20},{103}});
  skeleton.roadNodes.push_back({102,CityRoadNodeKind::Endpoint,{220,0,40},{103}});
  skeleton.roadEdges.push_back({103,1,101,102,CityRoadKind::FieldTrack,{{180,0,20},{200,0,20},{220,0,40}},.75});

  CityExteriorAllocation exterior;
  exterior.revision = 5;
  CityGrowthPlan growth;
  growth.revision = 5;
  CityGrowthWall old;
  old.id = old.era = 1;
  old.lifecycle = CityWallLifecycle::Absorbed;
  old.boundary = rect(0, 0, 40, 40);
  old.gates = {{0, 0, 20}};
  old.towers = {{0, 0, 0}, {40, 0, 40}};
  CityGrowthWall standing;
  standing.id = standing.era = 2;
  standing.lifecycle = CityWallLifecycle::Standing;
  standing.boundary = rect(-10, -10, 60, 60);
  standing.gates = {{-10, 0, 20}};
  standing.towers = {{-10, 0, -10}, {50, 0, 50}};
  CityGrowthWall planned;
  planned.id = planned.era = 3;
  planned.lifecycle = CityWallLifecycle::Planned;
  planned.boundary = rect(-20, -20, 100, 80);
  planned.gates = {{-20, 0, 20}};
  planned.towers = {{-20, 0, -20}, {80, 0, 60}};
  growth.walls = {standing};
  growth.traces = {old, planned};
  growth.zones.push_back({7, 0, CityGrowthZoneKind::Suburb, rect(50, 0, 25, 25), false, true});

  CityCastlePlan castles;
  castles.revision = 5;
  CityCastleTopology castle;
  castle.id = 70;
  castle.mode = CityCastleMode::WallAttached;
  castle.precinct = rect(-10, 5, 15, 20);
  castle.enclosure = {{-10, 0, 5}, {5, 0, 5}, {5, 0, 25}, {-10, 0, 25}};
  castles.castles.push_back(castle);

  {
    CityCompletionPlan crossing;
    crossing.skeleton.roadNodes={{1,CityRoadNodeKind::Endpoint,{-20,0,0},{1}}, {2,CityRoadNodeKind::Endpoint,{20,0,0},{1}}};
    crossing.skeleton.roadEdges={{1,1,1,2,CityRoadKind::Street,{{-20,0,0},{20,0,0}},2}};
    crossing.routes={{1,CityRoadKind::Street,{1},{{-20,0,0},{20,0,0}},2}};
    CityGrowthPlan dated;dated.historical=true;
    CityGrowthWall wall;wall.id=80;wall.era=1;wall.palisade=true;
    wall.boundary=rect(-10,-10,20,20);wall.gates={{-10,0,0},{10,0,0}};wall.gateWidths={8,6};dated.walls={wall};
    std::string why;
    check(realizeCityGrowthWalls(dated,{}, {},crossing,&why) && crossing.wallSegments.empty() &&
          crossing.gatehouses.empty() && crossing.skeleton.roadEdges.size()==1,
          "archival timber walls leave neither meshes nor road gate cuts");
    CityCompletionSettings timber;timber.timberDefences=true;
    check(realizeCityGrowthWalls(dated,{}, timber,crossing,&why),"explicit timber defence mode retains historical geometry");
    double roadLength=0;
    for(const auto &edge:crossing.skeleton.roadEdges)for(size_t i=1;i<edge.path.size();++i)
      roadLength+=std::hypot(edge.path[i].x-edge.path[i-1].x,edge.path[i].z-edge.path[i-1].z);
    check(crossing.skeleton.roadEdges.size()==3 && crossing.routes[0].edges.size()==3 && std::abs(roadLength-40)<1e-9,
      "gates split and reuse the existing road with complete route provenance");
    check(crossing.gatehouses.size()==2 && crossing.gatehouses[0].openingWidth>=6 &&
      std::none_of(crossing.gatehouses.begin(),crossing.gatehouses.end(),[](const auto &g){return g.built;}),
      "palisade crossings retain required widths without invented stone gatehouses");
    check(!crossing.wallSegments.empty() && std::all_of(crossing.wallSegments.begin(),crossing.wallSegments.end(),[](const auto &w){return w.palisade && w.height==3.5 && w.width==.6;}),
      "historical palisades use their own material and dimensions");
    dated.walls[0].openings={{0,0,.2},{0,.1,.3},{3,.8,1},{3,.35,.65},{1,.35,.65}};
    check(realizeCityGrowthWalls(dated,{}, timber,crossing,&why),"overlapping corner road openings compose");
    bool clearCorner=true;
    for(const auto &segment:crossing.wallSegments) {
      if(std::abs(segment.a.z+10)<1e-6 && std::abs(segment.b.z+10)<1e-6)
        clearCorner=clearCorner && std::min(segment.a.x,segment.b.x)>=-4.-1e-6;
      if(std::abs(segment.a.x+10)<1e-6 && std::abs(segment.b.x+10)<1e-6)
        clearCorner=clearCorner && std::min(segment.a.z,segment.b.z)>=-6.-1e-6;
    }
    check(clearCorner,"full-width overlapping road openings remove wall fragments on both sides of a corner");
  }

  CityCompletionPlan plan;
  std::string error;
  check(completeCityPlan(skeleton, exterior, growth, castles, {}, plan, &error), "complete city plan composes");
  check(plan.lots.size()==1 && plan.lots[0].merchantHouse && plan.lots[0].preferredFootprintMetres2==12 &&
        plan.lots[0].desiredRooms==3 && plan.lots[0].minimumFloors==2 && plan.lots[0].maximumFloors==3,
        "city allocation chooses programme and vertical capacity for a small merchant lot");
  {
    auto estates=skeleton;
    estates.regions.push_back({32,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::WealthyResidential,rect(0,30,15,16),false,240});
    estates.regions.push_back({33,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::WealthyResidential,rect(30,30,25,24),false,600});
    CityCompletionPlan allocated;
    check(completeCityPlan(estates,exterior,growth,castles,{},allocated,&error),"estate allocation composes");
    check(allocated.lots.size()==3 && allocated.lots[1].residence==CityResidenceKind::Mansion &&
          allocated.lots[2].residence==CityResidenceKind::Manor && allocated.lots[2].desiredRooms>allocated.lots[1].desiredRooms,
          "large residential lots select mansion and manor programmes, not fortifications");
    check(allocated.lots[1].preferredFootprintMetres2==140 && allocated.lots[2].preferredFootprintMetres2==200 &&
          allocated.lots[2].desiredRooms*allocated.lots[2].preferredRoomMetres2>400,
          "estates cap their ground coverage and budget rooms over multiple storeys");
    check(allocated.lots[1].maximumFloors==0 && allocated.lots[2].maximumFloors==0,"wealthy lots have no district height cap");
    estates.regions.push_back({34,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::WorkerHousing,rect(70,30,5,6),false,30});
    check(completeCityPlan(estates,exterior,growth,castles,{},allocated,&error),"poor housing policy composes");
    check(allocated.lots.back().minimumFloors==1 && allocated.lots.back().maximumFloors==1,"poor households cannot grow a second floor");
  }
  check(plan.history.size() == 2, "wall eras publish absorbed-by history");
  check(plan.gatehouses.size() == 2 && plan.skeleton.gates.size() == 2, "main and castle gates have gatehouses and graph contacts");
  check(plan.towers.size() == 6 && std::all_of(plan.towers.begin(), plan.towers.end(), [](const auto &t) { return t.built; }),
        "main and castle tower stations become structures");
  std::set<uint64_t> wallNodes, roadNodes;
  for (const auto &n : plan.skeleton.wallNodes) wallNodes.insert(n.id);
  for (const auto &n : plan.skeleton.roadNodes) roadNodes.insert(n.id);
  bool contacts = true;
  for (const auto &g : plan.skeleton.gates) contacts &= wallNodes.count(g.wallNode) && roadNodes.count(g.roadNode);
  check(contacts, "every gate names existing wall and road nodes");
  bool sixFaces = !plan.wallSegments.empty(), standingBuilt = false, sharedCastle = false;
  for (const auto &s : plan.wallSegments)
  {
    sixFaces &= s.faces.size() == 6 && s.width > 0 && s.height > 0 && s.depth > 0;
    standingBuilt |= s.lifecycle == CityWallLifecycle::Standing && s.built;
    sharedCastle |= s.sharedWithCastle;
  }
  check(sixFaces && standingBuilt, "wall structure stores six faces and built state");
  bool gateGap = true;
  for (const auto &gate : plan.gatehouses)
    for (const auto &segment : plan.wallSegments)
      if (segment.wall == gate.wall)
        gateGap &= std::min(std::hypot(segment.a.x - gate.position.x, segment.a.z - gate.position.z), std::hypot(segment.b.x - gate.position.x, segment.b.z - gate.position.z)) >= gate.openingWidth * .49;
  check(gateGap, "structural walls leave physical gate openings");
  check(sharedCastle, "castle-shared wall intervals are structural metadata");
  check(plan.suburbs.size() == 1 && plan.suburbs[0].accessNode && plan.suburbs[0].density > 0 && plan.suburbs[0].density <= 1, "suburb receives access and gate-distance density");
  check(!plan.routes.empty() && std::any_of(plan.routes.begin(), plan.routes.end(), [](const auto &r) { return r.edges.size() >= 2 && r.path.size() > 4; }),
        "degree-two road chains become locked-endpoint smooth routes");
  check(plan.harbours.size() == 1 && plan.harbours[0].quay.size() == 2 && plan.harbours[0].accessNode, "harbour district publishes quay and service-road contact");
  check(std::any_of(plan.routes.begin(),plan.routes.end(),[](const auto &r) {
    return r.kind==CityRoadKind::FieldTrack && r.halfWidth==.75 && r.path.size()>3 &&
           r.path.front().x==180 && r.path.back().x==220;
  }), "field tracks reach mesh routes with preserved width and locked endpoints");
  std::set<uint64_t> routed;
  for (const auto &route:plan.routes) for (auto id:route.edges) routed.insert(id);
  check(routed.size()==plan.skeleton.roadEdges.size(), "every planned road edge belongs to a mesh route");

  CityCompletionPlan repeat;
  check(completeCityPlan(skeleton, exterior, growth, castles, {}, repeat, &error) && repeat.wallSegments.size() == plan.wallSegments.size() && repeat.wallSegments.front().id == plan.wallSegments.front().id,
        "city completion is deterministic");

  {
    CityCompletionPlan c;c.revision=1;
    CitySmoothedRoute river;river.id=700;river.halfWidth=3.2;river.path={{0,0,0},{80,0,0}};river.edges={70,71};
    CitySmoothedRoute street;street.id=701;street.halfWidth=2;street.path={{40,0,-20},{40,0,20}};
    c.routes={river,street};
    CityWallStructureSegment wall;wall.id=90;wall.a={65,0,-15};wall.b={65,0,15};wall.width=1.5;wall.height=6;c.wallSegments={wall};
    CityReliefPlan r;r.ground=r.openGround=[](double,double){return 10.;};
    check(planCityWaterway(c,r,700),"wide level street can become an engineered waterway");
    check(c.waterways.size()==1 && c.waterways[0].bridges.size()==3 && c.waterways[0].portals.size()==1,"waterway plans street crossing and wall passage with end access");
    check(c.waterways[0].waterLevel>c.waterways[0].bedLevel && r.accessGround(30,0)<c.waterways[0].bedLevel && r.accessGround(30,4)==10,"waterway excavation stays below water and preserves quays");
    check(c.waterways[0].edges==river.edges,"waterway retains source road-edge ownership");
    check(c.waterways[0].portals.front().to-c.waterways[0].portals.front().from>river.halfWidth*2,"wall portal keeps both quays open beside the water");
    auto automatic=c;automatic.waterways.clear();automatic.wallSegments.clear();automatic.routes={river};automatic.routes[0].path={{0,0,0},{200,0,0}};
    CityWallTowerStructure tower;tower.position={0,0,0};tower.radius=5;automatic.towers={tower};
    CityReliefPlan automaticRelief;automaticRelief.openGround=[](double,double){return 10.;};
    check(planCityWaterway(automatic,automaticRelief) && automatic.waterways.front().a.x>8,"automatic selection finds an interior reach clear of a gate tower");
    auto steep=c;steep.waterways.clear();CityReliefPlan slope;slope.openGround=[](double x,double){return x*.1;};
    check(!planCityWaterway(steep,slope,700) && steep.waterways.empty() && !slope.accessGround,"steep canal candidate is rejected atomically");
    auto narrow=c;narrow.waterways.clear();narrow.routes[0].halfWidth=2;
    CityReliefPlan flat;flat.openGround=[](double,double){return 10.;};
    check(!planCityWaterway(narrow,flat,700),"narrow streets retain their public circulation");
    auto blocked=c;blocked.waterways.clear();CityReliefPlan stairs=flat;
    CityReliefPlan::Stairway stair;stair.a={30,0,-5};stair.b={30,4,5};stair.halfWidth=1.2;stairs.stairs.push_back(stair);
    check(!planCityWaterway(blocked,stairs,700),"waterway cannot intersect staircase clearance");
    blocked.skeleton.regions.push_back({1,1,0,CityRegionKind::Lot,CityLandUse::UrbanLot,DistrictUse::Residential,rect(20,-2,10,4),false,40});
    check(!planCityWaterway(blocked,flat,700),"waterway cannot consume a house lot");
    blocked.skeleton.regions.front().boundary=rect(-20,-20,120,40);
    check(!planCityWaterway(blocked,flat,700),"waterway cannot hide entirely inside a large lot");
  }

  std::cout << "CITY_COMPLETION_TESTS failures=" << failures << '\n';
  return failures ? 1 : 0;
}
