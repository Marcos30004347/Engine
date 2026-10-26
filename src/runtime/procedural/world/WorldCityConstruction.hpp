#pragma once
#include "WorldCitySkeleton.hpp"
#include <map>
#include <set>
#include <algorithm>
#include <cmath>

namespace procedural::world::rewrite {
// Source-domain IDs avoid collisions between a road, region and historical
// building with the same numeric ID. No vector index is an identity.
enum class CityConstructionKind { City, District, Complex, Plot, Dwelling, Space, Route, Platform, Wall, RoadJunction };
struct CityConstructionId {
  CityConstructionKind kind=CityConstructionKind::City;
  uint64_t source=0;
  bool operator<(const CityConstructionId &b) const {
    return kind!=b.kind ? kind<b.kind : source<b.source;
  }
  bool operator==(const CityConstructionId &b) const { return kind==b.kind && source==b.source; }
};
enum class CityComplexGrammar { None, Hillside, Castle, Gatehouse, Market, Quay, Bridge };
enum class CitySpaceRole { None, Court, Approach, Service, Passage, Waterfront };
enum class CityInterfaceKind { TerrainSeam, Entrance, Support, WallSocket, Bridge, WaterCrossing };
struct CityComplexRule {
  std::vector<CitySpaceRole> requiredSpaces;
  bool requiresSupport=false;
};
inline CityComplexRule cityComplexRule(CityComplexGrammar grammar) {
  switch(grammar) {
    case CityComplexGrammar::Hillside:return {{CitySpaceRole::Court,CitySpaceRole::Approach},false};
    case CityComplexGrammar::Castle:return {{CitySpaceRole::Court,CitySpaceRole::Approach,CitySpaceRole::Service},false};
    case CityComplexGrammar::Gatehouse:return {{CitySpaceRole::Passage,CitySpaceRole::Approach},false};
    case CityComplexGrammar::Market:return {{CitySpaceRole::Court,CitySpaceRole::Service},false};
    case CityComplexGrammar::Quay:return {{CitySpaceRole::Waterfront,CitySpaceRole::Approach},false};
    case CityComplexGrammar::Bridge:return {{CitySpaceRole::Passage,CitySpaceRole::Approach},true};
    default:return {};
  }
}
struct CityConstructionElement {
  CityConstructionId id, owner;
  CityComplexGrammar grammar=CityComplexGrammar::None;
  CitySpaceRole role=CitySpaceRole::None;
  std::vector<Position> boundary;
  double capacityArea=0;
  bool arrival=false, requiresAccess=false;
};
struct CityConstructionConnection {
  uint64_t id=0;
  CityConstructionId a,b;
  double width=0;
};
struct CityConstructionSupport { CityConstructionId supported, support; };
struct CityConstructionPatch {
  uint64_t id=0;
  CityConstructionId owner;
  std::vector<Position> boundary;
};
struct CityConstructionDemand {
  uint64_t building=0;
  double requiredArea=0;
  CityConstructionId allocation;
  bool placed=false;
  std::string unmetReason;
};
struct CityConstructionInterface {
  uint64_t id=0;
  CityInterfaceKind kind=CityInterfaceKind::TerrainSeam;
  CityConstructionId a,b,owner;
  uint64_t acceptedA=0,acceptedB=0; // Both accept this blueprint revision.
  std::vector<Position> stations; // One canonical seam, never two rounded copies.
  double width=0, bottom=0, top=0;
};
struct CityConstructionVolume {
  enum class Use { Solid, Floor, Clearance, Support };
  uint64_t id=0;
  CityConstructionId owner;
  Use use=Use::Solid;
  std::vector<Position> footprint;
  double bottom=0,top=0;
};
struct CityConstructionIssue { std::string code; CityConstructionId element; };
struct CityConstructionPlan {
  uint64_t revision=0;
  std::vector<CityConstructionElement> elements;
  std::vector<CityConstructionConnection> access;
  std::vector<CityConstructionSupport> supports;
  std::vector<CityConstructionPatch> patches;
  std::vector<CityConstructionDemand> demand;
  std::vector<CityConstructionInterface> interfaces;
  std::vector<CityConstructionVolume> volumes;
  // Legacy adapters produce candidates, not certificates of geometric safety.
  std::vector<CityConstructionIssue> issues;
};
inline bool validateCityConstruction(const CityConstructionPlan &p,std::vector<CityConstructionIssue> &issues) {
  issues.clear();
  auto reject=[&](const char *code,CityConstructionId id=CityConstructionId{}){issues.push_back({code,id});};
  std::map<CityConstructionId,const CityConstructionElement *> elements;
  size_t roots=0;
  const auto finite=[](Position v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
  if(!p.revision)reject("missing_revision");
  for(const auto &e:p.elements) {
    if(!e.id.source || !elements.emplace(e.id,&e).second)reject("duplicate_or_zero_identity",e.id);
    if(e.id.kind==CityConstructionKind::City)++roots;
    if(!std::isfinite(e.capacityArea)||e.capacityArea<0)reject("invalid_capacity",e.id);
    for(auto v:e.boundary)if(!finite(v))reject("nonfinite_boundary",e.id);
  }
  if(roots!=1)reject("requires_one_city_root");
  for(const auto &e:p.elements) {
    using K=CityConstructionKind;
    if(e.grammar!=CityComplexGrammar::None && e.id.kind!=K::Complex)reject("grammar_requires_complex",e.id);
    if((e.id.kind==K::District && e.owner.kind!=K::City) ||
       (e.id.kind==K::Complex && e.owner.kind!=K::District) ||
       (e.id.kind==K::Plot && e.owner.kind!=K::Complex) ||
       (e.id.kind==K::Dwelling && e.owner.kind!=K::Plot))reject("invalid_hierarchy",e.id);
    std::set<CityConstructionId> seen;
    auto current=&e;
    while(current->id.kind!=CityConstructionKind::City) {
      if(!seen.insert(current->id).second){reject("ownership_cycle",e.id);break;}
      auto parent=elements.find(current->owner);
      if(parent==elements.end()){reject("missing_owner",e.id);break;}
      current=parent->second;
    }
  }
  std::map<CityConstructionId,std::vector<CityConstructionId>> adjacency,support;
  std::set<uint64_t> ids;
  for(const auto &c:p.access) {
    if(!c.id||!ids.insert(c.id).second)reject("duplicate_access_identity",c.a);
    if(!elements.count(c.a)||!elements.count(c.b)||c.a==c.b)reject("invalid_access_endpoint",c.a);
    if(!std::isfinite(c.width)||c.width<=0)reject("invalid_access_width",c.a);
    adjacency[c.a].push_back(c.b);adjacency[c.b].push_back(c.a);
  }
  std::set<CityConstructionId> reachable;std::vector<CityConstructionId> queue;
  for(const auto &e:p.elements)if(e.arrival){reachable.insert(e.id);queue.push_back(e.id);}
  for(size_t i=0;i<queue.size();++i)for(auto next:adjacency[queue[i]])
    if(reachable.insert(next).second)queue.push_back(next);
  for(const auto &e:p.elements)if((e.requiresAccess||e.id.kind==CityConstructionKind::Platform)&&!reachable.count(e.id))
    reject("unreachable_required_space",e.id);
  for(const auto &s:p.supports) {
    if(!elements.count(s.supported)||!elements.count(s.support))reject("missing_support_endpoint",s.supported);
    support[s.supported].push_back(s.support);
  }
  // Kahn's algorithm: bounded stack even for very large complexes.
  std::map<CityConstructionId,size_t> degree;
  for(const auto &[id,e]:elements)degree[id]=0;
  for(const auto &[id,links]:support)for(auto next:links)++degree[next];
  queue.clear();for(const auto &[id,d]:degree)if(!d)queue.push_back(id);
  for(size_t i=0;i<queue.size();++i)for(auto next:support[queue[i]])if(--degree[next]==0)queue.push_back(next);
  if(queue.size()!=degree.size())reject("support_cycle");
  for(const auto &e:p.elements) {
    if(e.id.kind==CityConstructionKind::Platform) {
      bool used=false;for(const auto &s:p.supports)used|=s.support==e.id;
      if(!used)reject("unused_platform",e.id);
    }
    auto rule=cityComplexRule(e.grammar);
    for(auto role:rule.requiredSpaces) {
      bool found=false;for(const auto &child:p.elements)
        found|=child.id.kind==CityConstructionKind::Space && child.owner==e.id && child.role==role && reachable.count(child.id);
      if(!found)reject("missing_connected_grammar_space",e.id);
    }
    if(rule.requiresSupport && support[e.id].empty())reject("unsupported_complex",e.id);
  }
  ids.clear();for(const auto &patch:p.patches) {
    if(!patch.id||!ids.insert(patch.id).second)reject("duplicate_ground_patch",patch.owner);
    if(!elements.count(patch.owner))reject("missing_patch_owner",patch.owner);
    double area=0;
    for(size_t i=0;i<patch.boundary.size();++i) {
      auto a=patch.boundary[i],b=patch.boundary[(i+1)%patch.boundary.size()];
      if(!finite(a))reject("nonfinite_patch",patch.owner);
      area+=a.x*b.z-b.x*a.z;
    }
    if(patch.boundary.size()<3||std::abs(area)<1e-8)reject("degenerate_ground_patch",patch.owner);
  }
  ids.clear();std::map<CityConstructionId,double> allocated;
  for(const auto &d:p.demand) {
    if(!d.building||!ids.insert(d.building).second)reject("duplicate_historical_request",d.allocation);
    if(!std::isfinite(d.requiredArea)||d.requiredArea<=0)reject("invalid_programme_area",d.allocation);
    if(d.placed) {
      if(!elements.count(d.allocation))reject("missing_programme_allocation",d.allocation);
      allocated[d.allocation]+=d.requiredArea;
    } else if(d.unmetReason.empty())reject("unaccounted_programme",d.allocation);
  }
  for(auto [id,area]:allocated)if(elements.count(id)&&area>elements[id]->capacityArea+1e-6)reject("programme_capacity_exceeded",id);
  ids.clear();for(const auto &c:p.interfaces) {
    if(!c.id||!ids.insert(c.id).second)reject("duplicate_interface",c.owner);
    if(!elements.count(c.a)||!elements.count(c.b)||c.a==c.b || !(c.owner==c.a||c.owner==c.b))reject("invalid_interface_ownership",c.owner);
    if(c.acceptedA!=p.revision||c.acceptedB!=p.revision)reject("unaccepted_interface_revision",c.owner);
    if(c.stations.empty())reject("missing_interface_stations",c.owner);
    for(auto v:c.stations)if(!finite(v))reject("nonfinite_interface",c.owner);
    if(!std::isfinite(c.width)||c.width<0||!std::isfinite(c.bottom)||!std::isfinite(c.top)||c.top<c.bottom)reject("invalid_interface_dimensions",c.owner);
  }
  ids.clear();for(const auto &v:p.volumes) {
    if(!v.id||!ids.insert(v.id).second)reject("duplicate_volume",v.owner);
    if(!elements.count(v.owner))reject("missing_volume_owner",v.owner);
    if(v.footprint.size()<3||!std::isfinite(v.bottom)||!std::isfinite(v.top)||v.top<v.bottom)
      reject("invalid_volume",v.owner);
    for(auto point:v.footprint)if(!finite(point))reject("nonfinite_volume",v.owner);
  }
  return issues.empty();
}
// Publication is transactional: a failed candidate cannot replace a valid plan.
inline bool publishCityConstruction(CityConstructionPlan candidate,CityConstructionPlan &result,
                                    std::vector<CityConstructionIssue> &issues) {
  if(!validateCityConstruction(candidate,issues))return false;
  candidate.issues.clear();result=std::move(candidate);return true;
}
}
