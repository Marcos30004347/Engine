#pragma once
#include "WorldCityPlotContracts.hpp"
#include "WorldCityTerrainRoutesIntegration.hpp"
#include "procedural/geom2/Boolean.hpp"
namespace procedural::world::rewrite {
inline bool applyCityPlotContracts(const CityPlotContractPlan &contracts,CityCompletionPlan &city,
    CityReliefPlan &relief,std::string *error=nullptr) {
  const auto fail=[&](const char *s){if(error)*error=s;return false;};
  if(!contracts.revision||!contracts.terrain||contracts.terrain->revision!=contracts.revision||
     contracts.requested!=contracts.plots.size()+contracts.unmet.size() ||
     (contracts.requested && double(contracts.plots.size())/contracts.requested<.9))
    return fail("plot capacity or revision gate failed before meshing");
  std::set<uint64_t> requests,blocks,other;
  for(const auto &r:city.skeleton.regions)if(r.kind==CityRegionKind::Block)blocks.insert(r.id);
  for(const auto &r:city.skeleton.regions)if(r.kind!=CityRegionKind::Lot)other.insert(r.id);
  for(const auto &c:contracts.plots)if(!c.building||!requests.insert(c.building).second||other.count(c.building)||!blocks.count(c.block))return fail("invalid plot identity or block owner");
  for(const auto &u:contracts.unmet)if(!u.building||!requests.insert(u.building).second||u.reason.empty())return fail("unaccounted plot demand");
  for(const auto &b:city.historicalLots)if(!requests.count(b.building))return fail("plot solve dropped historical demand");
  for(const auto &c:contracts.plots) {
    if(!c.brief.capacity || c.lot.size()<3 || !std::isfinite(c.floor) || !std::isfinite(c.requestedArea) || c.requestedArea<=0)
      return fail("incomplete plot contract");
    const auto block=std::find_if(city.skeleton.regions.begin(),city.skeleton.regions.end(),[&](const auto &r){return r.id==c.block;});
    geom2::Polygon parent,lot;
    for(auto p:block->boundary)parent.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
    for(auto p:c.lot)lot.outer.push_back({geom2::toUnit(p.x),geom2::toUnit(p.z)});
    std::vector<geom2::Polygon> outside;std::string why;
    if(!geom2::differenceOf(geom2::toOperand(lot),{geom2::toOperand(parent)},outside,&why))return fail("invalid contracted lot boundary");
    for(const auto &p:outside)if(std::abs(geom2::areaMetres(p))>1e-6)return fail("contracted lot exceeds its parent block");
  }
  auto next=city;auto levels=relief;
  if(!applyCityTerrainRoutes(*contracts.terrain,next,levels,error))return false;
  levels.constructionTerrain=contracts.terrain;
  std::set<uint64_t> oldLots;for(const auto &r:next.skeleton.regions)if(r.kind==CityRegionKind::Lot)oldLots.insert(r.id);
  next.skeleton.regionContacts.erase(std::remove_if(next.skeleton.regionContacts.begin(),next.skeleton.regionContacts.end(),
    [&](const auto &c){return oldLots.count(c.a)||oldLots.count(c.b);}),next.skeleton.regionContacts.end());
  next.skeleton.regionAccess.erase(std::remove_if(next.skeleton.regionAccess.begin(),next.skeleton.regionAccess.end(),
    [&](const auto &a){return oldLots.count(a.region);}),next.skeleton.regionAccess.end());
  next.skeleton.regions.erase(std::remove_if(next.skeleton.regions.begin(),next.skeleton.regions.end(),[](const auto &r){return r.kind==CityRegionKind::Lot;}),next.skeleton.regions.end());
  next.lots.clear();next.historicalLots.clear();
  for(const auto &c:contracts.plots) {
    CitySkeletonRegion r;r.id=c.building;r.parent=c.block;r.source=c.building;r.kind=CityRegionKind::Lot;
    r.landUse=CityLandUse::UrbanLot;r.boundary=c.lot;r.area=city_plot_detail::area(c.lot);next.skeleton.regions.push_back(r);
    CityLotDevelopment l;l.lot=c.building;l.preferredFootprintMetres2=c.requestedArea;l.desiredRooms=c.brief.capacity->desiredRooms;
    l.minimumFloors=l.maximumFloors=c.brief.floors;next.lots.push_back(l);
    next.historicalLots.push_back({c.building,c.building,c.requestedArea});
    levels.regionLevel[c.building]=c.floor;
  }
  for(const auto &u:contracts.unmet)next.historicalLots.push_back({u.building,0,u.requiredArea});
  next.plotContracts=std::make_shared<const CityPlotContractPlan>(contracts);
  city=std::move(next);relief=std::move(levels);if(error)error->clear();return true;
}
}
