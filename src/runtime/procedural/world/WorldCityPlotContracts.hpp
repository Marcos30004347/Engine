#pragma once
#include "WorldCityTerrainRoutes.hpp"
#include "generator/CellDwellingPlanner.hpp"
#include "generator/CellDwellingRealise.hpp"
namespace procedural::world::rewrite {
struct CityPlotRequest {
  uint64_t building=0;
  double footprintArea=40;
  uint32_t rooms=2,floors=1;
  generator::BuildingClass buildingClass=generator::BuildingClass::Dwelling;
};
struct CityPlotFrontage {
  uint64_t block=0,road=0;
  std::vector<Position> bench; // Convex reserved land; first edge sets modular axis.
};
struct CityPlotSettings {
  double module=1.2,approachWidth=1.2,floorClearance=.2,maximumRise=2;
  double minimumBuildingGap=0; // Explicit frontage spacing; never rescales cells.
  double maximumStepRise=.18,minimumGoing=.3,minimumPlacement=.9;
  uint32_t maximumCellsPerSide=16;
  size_t maximumAttempts=512;
};
struct CityPlotStep { std::vector<Position> boundary;double bottom=0,top=0; };
struct CityPlotContract {
  uint64_t building=0,block=0,road=0;
  double requestedArea=0,floor=0,sidewalk=0,module=0,doorWidth=0;
  Position door,street;
  std::vector<Position> lot,envelope,approach;
  std::vector<CityPlotStep> steps;
  generator::CellDwellingBrief brief;
  generator::CellDwellingSettings settings;
  generator::CellHouse house;
  DwellingPlan dwelling; // Authoritative layout, reused by the mesh builder.
};
struct CityPlotUnmet {uint64_t building=0;std::string reason;double requiredArea=0;};
struct CityPlotContractPlan {
  uint64_t revision=0;
  std::shared_ptr<const CityTerrainRoutePlan> terrain;
  std::vector<CityPlotContract> plots;
  std::vector<CityPlotUnmet> unmet;
  size_t requested=0,attempts=0;
};
namespace city_plot_detail {
inline double area(const std::vector<Position> &ring) {
  double a=0;for(size_t i=0;i<ring.size();++i){auto p=ring[i],q=ring[(i+1)%ring.size()];a+=p.x*q.z-p.z*q.x;}return std::abs(a)*.5;
}
inline bool overlap(const std::vector<Position> &a,const std::vector<Position> &b) {
  return area(city_terrain_detail::intersect(a,b))>1e-6;
}
inline std::vector<Position> ribbon(Position a,Position b,double width) {
  double x=b.x-a.x,z=b.z-a.z,l=std::hypot(x,z);if(l<1e-8)return {};
  double nx=-z/l*width*.5,nz=x/l*width*.5;
  return {{a.x-nx,0,a.z-nz},{b.x-nx,0,b.z-nz},{b.x+nx,0,b.z+nz},{a.x+nx,0,a.z+nz}};
}
}
// First-fit packing on full dwelling modules. Small alternatives change the
// footprint aspect and position, never module scale, room demand or floor count.
// Result publication is transactional; report always exposes unmet identities.
inline bool planCityPlotContracts(const CityTerrainRoutePlan &terrain,
    const std::vector<CityPlotFrontage> &frontages,const std::vector<CityPlotRequest> &requests,
    const std::vector<CityTerrainReservation> &protectedLand,const CityPlotSettings &settings,
    CityPlotContractPlan &result,CityPlotContractPlan &report,std::string *error=nullptr) {
  using namespace city_terrain_detail;using namespace city_plot_detail;
  const auto fail=[&](const char *s){if(error)*error=s;return false;};report={};
  if(!terrain.revision||!terrain.ground||!std::isfinite(settings.module+settings.approachWidth+settings.floorClearance+
      settings.minimumBuildingGap+settings.maximumRise+settings.maximumStepRise+settings.minimumGoing+settings.minimumPlacement)||
      settings.minimumBuildingGap<0||settings.minimumBuildingGap>8||settings.module<1||settings.module>4||settings.approachWidth<.9||settings.floorClearance<.1||settings.maximumRise<0||
      settings.maximumStepRise<=0||settings.minimumGoing<=0||settings.minimumPlacement<.9||settings.minimumPlacement>1||
      settings.maximumCellsPerSide<4||settings.maximumCellsPerSide>64||!settings.maximumAttempts)
    return fail("invalid plot contract settings");
  auto demand=requests;std::set<uint64_t> ids;
  for(const auto &r:demand)if(!r.building||!ids.insert(r.building).second||!std::isfinite(r.footprintArea)||r.footprintArea<=0||!r.rooms||r.rooms>128||!r.floors||r.floors>16)
    return fail("invalid historical plot request");
  for(const auto &r:protectedLand)if(!convex(r.boundary))return fail("invalid protected plot polygon");
  ids.clear();for(const auto &f:frontages)if(!f.block||!ids.insert(f.block).second||!convex(f.bench))return fail("invalid plot frontage");
  std::sort(demand.begin(),demand.end(),[](const auto &a,const auto &b){return a.footprintArea!=b.footprintArea?a.footprintArea>b.footprintArea:a.building<b.building;});
  auto fronts=frontages;std::sort(fronts.begin(),fronts.end(),[](const auto &a,const auto &b){return a.block<b.block;});
  CityPlotContractPlan plan;plan.revision=terrain.revision;plan.requested=requests.size();
  plan.terrain=std::make_shared<const CityTerrainRoutePlan>(terrain);
  for(const auto &request:demand) {
    bool accepted=false;std::string reason="no modular frontage capacity";
    for(const auto &front:fronts) {
      const auto road=std::find_if(terrain.profiles.streets.begin(),terrain.profiles.streets.end(),[&](const auto &r){return r.edge==front.road;});
      const auto roadEdge=std::find_if(terrain.roads.roadEdges.begin(),terrain.roads.roadEdges.end(),[&](const auto &r){return r.id==front.road;});
      if(road==terrain.profiles.streets.end()||roadEdge==terrain.roads.roadEdges.end()){reason="missing serving street";continue;}
      auto origin=front.bench[0],end=front.bench[1];double length=std::hypot(end.x-origin.x,end.z-origin.z);
      if(length<settings.module)continue;double ux=(end.x-origin.x)/length,uz=(end.z-origin.z)/length;
      const auto at=[&](double u,double v){return Position{origin.x+ux*u-uz*v,0,origin.z+uz*u+ux*v};};
      double maxU=0,maxV=0;for(auto p:front.bench){maxU=std::max(maxU,(p.x-origin.x)*ux+(p.z-origin.z)*uz);maxV=std::max(maxV,-(p.x-origin.x)*uz+(p.z-origin.z)*ux);}
      if(maxU/settings.module>1000||maxV/settings.module>1000){reason="frontage search bounds exceeded";continue;}
      if(request.footprintArea>settings.module*settings.module*settings.maximumCellsPerSide*settings.maximumCellsPerSide){reason="programme exceeds modular envelope capacity";continue;}
      const uint32_t needed=uint32_t(std::ceil(request.footprintArea/(settings.module*settings.module)));
      for(uint32_t width=4;width<=settings.maximumCellsPerSide&&!accepted;++width) {
        uint32_t depth=std::max(4u,(needed+width-1)/width);
        if(depth>settings.maximumCellsPerSide)continue;
        for(double v=0;v+depth*settings.module<=maxV+1e-7&&!accepted;v+=settings.module)
          for(double u=0;u+width*settings.module<=maxU+1e-7&&!accepted;u+=settings.module) {
            if(plan.attempts>=settings.maximumAttempts){break;}
            ++plan.attempts;
            CityPlotContract c;c.building=request.building;c.block=front.block;c.road=front.road;c.module=settings.module;c.requestedArea=request.footprintArea;
            c.lot={at(u,v),at(u+width*settings.module,v),at(u+width*settings.module,v+depth*settings.module),at(u,v+depth*settings.module)};
            bool clear=true;for(auto p:c.lot)clear&=inside(front.bench,p);
            for(const auto &r:protectedLand)clear&=!overlap(c.lot,r.boundary);
            const double gap=settings.minimumBuildingGap;
            const std::vector<Position> separation={at(u-gap,v-gap),at(u+width*settings.module+gap,v-gap),at(u+width*settings.module+gap,v+depth*settings.module+gap),at(u-gap,v+depth*settings.module+gap)};
            for(const auto &p:plan.plots)clear&=!overlap(separation,p.lot)&&!overlap(c.lot,p.approach);
            if(!clear)continue;
            double low=INFINITY,high=-INFINITY;
            for(double x=0;x<=width;x+=.5)for(double z=0;z<=depth;z+=.5){auto p=at(u+x*settings.module,v+z*settings.module);double h=terrain.ground(p.x,p.z);low=std::min(low,h);high=std::max(high,h);if(!std::isfinite(h))clear=false;}
            if(!clear||high-low>.02){reason="lot needs a horizontal terrain bench";continue;}
            c.floor=high+settings.floorClearance;
            auto &brief=c.brief;brief.id=request.building;brief.seed=request.building;brief.building=request.buildingClass;
            for(auto p:c.lot)brief.lot.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
            brief.frontage={0};brief.floors=request.floors;
            brief.capacity=generator::CellDwellingBrief::Capacity{area(c.lot),request.rooms,15,true,false,4,request.floors};
            geom2::CellFrame frame;frame.originX=c.lot[0].x;frame.originZ=c.lot[0].z;frame.axisX=ux;frame.axisZ=uz;frame.cellMetres=settings.module;brief.sharedGrid=frame;
            c.settings.capacityCellMetres=settings.module;c.settings.maximumFloors=request.floors;c.settings.eavesOverhangMetres=0;
            c.settings.entranceFlightCells=0;c.settings.entrancePlatformRise=0;c.settings.footprintAttempts=8;
            generator::Ledger ledger;std::string why;
            if(!generator::planCellDwelling(brief,c.settings,c.house,ledger,&why)||
               !generator::realiseCellDwelling(c.house,c.settings,c.dwelling,&why)){reason="dwelling programme does not fit: "+why;continue;}
            if(c.dwelling.storeys.empty()||std::abs(c.house.frame.cellMetres-settings.module)>1e-7){reason="dwelling changed module scale";continue;}
            size_t rooms=0;for(const auto &floor:c.house.floors)rooms+=floor.roomAreas.size();
            if(rooms<request.rooms||c.house.floors.size()!=request.floors){reason="dwelling room or floor programme below request";continue;}
            c.envelope=c.dwelling.storeys.front().outer;
            if(area(c.envelope)+1e-5<request.footprintArea){reason="dwelling footprint below requested area";continue;}
            for(const auto &storey:c.dwelling.storeys)for(auto p:storey.outer)clear&=inside(c.lot,p);
            if(!clear){reason="dwelling exceeds modular envelope";continue;}
            bool access=false;
            for(const auto &o:c.dwelling.openings) {
              if(!o.exterior||o.kind!=OpeningKind::Door||o.floor!=0)continue;
              Position door{(o.a.x+o.b.x)*.5,c.floor+o.a.y+o.base,(o.a.z+o.b.z)*.5};
              double doorWidth=std::hypot(o.b.x-o.a.x,o.b.z-o.a.z);
              if(doorWidth<.85||std::abs(door.y-c.floor)>.02)continue;
              double best=INFINITY;Position street;
              for(size_t i=1;i<road->path.size();++i) {
                auto a=road->path[i-1],b=road->path[i];double dx=b.x-a.x,dz=b.z-a.z,l=dx*dx+dz*dz;
                double t=l>0?std::clamp(((door.x-a.x)*dx+(door.z-a.z)*dz)/l,0.,1.):0;auto p=lerp(a,b,t);
                double d=std::hypot(door.x-p.x,door.z-p.z);if(d<best){best=d;street=p;}
              }
              if(best<=roadEdge->halfWidth+.1||!std::isfinite(best))continue;
              double sx=(door.x-street.x)/best,sz=(door.z-street.z)/best;
              street.x+=sx*roadEdge->halfWidth;street.z+=sz*roadEdge->halfWidth;street.y=terrain.ground(street.x,street.z)+.035;
              double run=std::hypot(door.x-street.x,door.z-street.z),rise=door.y-street.y;
              if(rise<-.02||rise>settings.maximumRise)continue;
              auto approach=ribbon(street,door,settings.approachWidth);
              bool free=!overlap(c.envelope,approach);
              for(const auto &r:protectedLand)free&=!overlap(approach,r.boundary);
              for(const auto &p:plan.plots)free&=!overlap(approach,p.lot)&&!overlap(approach,p.approach);
              const unsigned steps=std::max(1u,unsigned(std::ceil(std::max(0.,rise)/settings.maximumStepRise)));
              if(!free||run/steps<settings.minimumGoing)continue;
              std::vector<CityPlotStep> treads;
              for(unsigned i=0;i<steps;++i) {
                auto a=lerp(street,door,double(i)/steps),b=lerp(street,door,double(i+1)/steps);
                auto ring=ribbon(a,b,settings.approachWidth);double top=b.y,bottom=INFINITY;
                // Check the full width, not just the route centreline.
                for(unsigned k=0;k<=4;++k)for(unsigned j=0;j<=4;++j){auto p=lerp(lerp(ring[0],ring[1],j/4.),lerp(ring[3],ring[2],j/4.),k/4.);double h=terrain.ground(p.x,p.z);bottom=std::min(bottom,h-.1);free&=std::isfinite(h)&&h+.035<=top+1e-6;}
                treads.push_back({ring,bottom,top});
              }
              if(!free)continue;
              c.door=door;c.doorWidth=doorWidth;c.street=street;c.sidewalk=street.y;c.approach=std::move(approach);c.steps=std::move(treads);access=true;break;
            }
            if(!access){reason="no clear doorway approach at agreed floor level";continue;}
            plan.plots.push_back(std::move(c));accepted=true;
          }
        if(plan.attempts>=settings.maximumAttempts)break;
      }
      if(accepted||plan.attempts>=settings.maximumAttempts)break;
    }
    if(!accepted)plan.unmet.push_back({request.building,(plan.attempts>=settings.maximumAttempts?"plot search budget exhausted: ":"")+reason,request.footprintArea});
  }
  report=plan;
  if(!requests.empty()&&double(plan.plots.size())/requests.size()+1e-12<settings.minimumPlacement)
    return fail("plot capacity gate failed before mesh generation");
  result=std::move(plan);if(error)error->clear();return true;
}
}
