#pragma once
#include "WorldCityConstruction.hpp"
#include "WorldCityCompletion.hpp"
#include "WorldCityRelief.hpp"
#include <cstring>
namespace procedural::world::rewrite {
// Read-only projection after historical allocation. This is intentionally a
// candidate: old frontages have not negotiated entrance/terrain contracts yet.
inline CityConstructionPlan cityConstructionCandidate(const CityCompletionPlan &city,const CityReliefPlan *relief=nullptr) {
  CityConstructionPlan p;p.revision=city.revision;
  const CityConstructionId root{CityConstructionKind::City,1};
  p.elements.push_back({root,root});
  std::map<uint64_t,CityConstructionId> regions;
  for(const auto &r:city.skeleton.regions) {
    const auto kind=r.kind==CityRegionKind::District?CityConstructionKind::District:
      r.kind==CityRegionKind::Lot?CityConstructionKind::Plot:CityConstructionKind::Complex;
    regions[r.id]={kind,r.id};
  }
  for(const auto &r:city.skeleton.regions) {
    CityConstructionElement e;e.id=regions[r.id];
    e.owner=regions.count(r.parent)?regions[r.parent]:root;e.boundary=r.boundary;
    e.requiresAccess=r.kind==CityRegionKind::Lot;
    double area=0;
    for(size_t i=0;i<r.boundary.size();++i){auto a=r.boundary[i],b=r.boundary[(i+1)%r.boundary.size()];area+=a.x*b.z-b.x*a.z;}
    e.capacityArea=std::abs(area)*.5;p.elements.push_back(e);
    // Only leaf plot footprints own ground here, not their containing district.
    if(r.kind==CityRegionKind::Lot)p.patches.push_back({r.id,e.id,r.boundary});
  }
  for(const auto &n:city.skeleton.roadNodes) {
    CityConstructionElement e;e.id={CityConstructionKind::RoadJunction,n.id};e.owner=root;
    e.arrival=n.kind==CityRoadNodeKind::Gate || n.kind==CityRoadNodeKind::Endpoint;
    e.role=CitySpaceRole::Approach;e.boundary={n.position};p.elements.push_back(e);
  }
  for(const auto &r:city.skeleton.roadEdges)
    p.access.push_back({r.id,{CityConstructionKind::RoadJunction,r.from},{CityConstructionKind::RoadJunction,r.to},2*r.halfWidth});
  // Region accesses are typed entrance contracts, not guessed nearest-road links.
  for(const auto &a:city.skeleton.regionAccess)if(regions.count(a.region)) {
    CityConstructionInterface c;c.id=a.id;c.kind=CityInterfaceKind::Entrance;
    c.a=regions[a.region];c.b={CityConstructionKind::RoadJunction,a.roadNode};c.owner=c.a;c.stations={a.point};
    p.interfaces.push_back(c); // Unaccepted until the joint level/approach solve.
  }
  // Public-space source data has no ID. Canonical vertex bits give the same
  // identity after ring rotation/reversal or input reordering; validation catches
  // duplicate authored spaces. A changed footprint is a new candidate space.
  for(const auto &space:city.skeleton.publicSpaces) {
    std::vector<std::array<uint64_t,3>> vertices;
    for(auto v:space.boundary) {
      std::array<uint64_t,3> bits{};double coords[]={v.x==0?0:v.x,v.y==0?0:v.y,v.z==0?0:v.z};
      for(size_t i=0;i<3;++i)std::memcpy(&bits[i],&coords[i],sizeof(double));
      vertices.push_back(bits);
    }
    std::sort(vertices.begin(),vertices.end());uint64_t id=1469598103934665603ULL;
    const auto mix=[&](uint64_t value){id^=value;id*=1099511628211ULL;};
    mix(space.owner);mix(uint64_t(space.kind));for(auto v:vertices)for(auto x:v)mix(x);
    CityConstructionElement e;e.id={CityConstructionKind::Space,id?id:1};
    e.owner=regions.count(space.owner)?regions[space.owner]:root;e.boundary=space.boundary;
    e.role=space.kind==CityPublicSpace::Kind::Court?CitySpaceRole::Court:CitySpaceRole::Service;
    e.requiresAccess=true;p.elements.push_back(e);
  }
  if(relief)for(const auto &deck:relief->platforms) {
    CityConstructionElement e;e.id={CityConstructionKind::Platform,deck.block};
    e.owner=regions.count(deck.block)?regions[deck.block]:root;e.boundary=deck.boundary;
    e.requiresAccess=true;p.elements.push_back(e);
    p.volumes.push_back({deck.block,e.id,CityConstructionVolume::Use::Support,deck.boundary,deck.foundation,deck.level});
    for(const auto &r:city.skeleton.regions)if(r.kind==CityRegionKind::Lot&&r.parent==deck.block)
      p.supports.push_back({regions[r.id],e.id});
    // Existing access paths are not certified entrances: phase 3 must negotiate
    // their endpoints and clearance before they enter the accepted access graph.
  }
  std::map<uint64_t,const CityLotDevelopment *> lots;
  for(const auto &l:city.lots)lots[l.lot]=&l;
  for(const auto &b:city.historicalLots) {
    CityConstructionDemand d;d.building=b.building;
    // Area is supplied by historical allocation before fitting, never by
    // measuring the available plot. Old bindings remain explicitly unresolved.
    d.requiredArea=b.requiredFootprintArea;
    if(regions.count(b.lot)&&lots.count(b.lot)&&lots[b.lot]->developed) {
      d.allocation=regions[b.lot];d.placed=true;
    } else d.unmetReason="No developed lot allocated";
    p.demand.push_back(d);
  }
  validateCityConstruction(p,p.issues);
  return p;
}
}
