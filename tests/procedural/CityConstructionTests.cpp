#include "procedural/world/WorldCityConstructionAdapter.hpp"
#include <iostream>
using namespace procedural::world::rewrite;
using K=CityConstructionKind;
static CityConstructionPlan fixture() {
  CityConstructionPlan p;p.revision=7;
  const CityConstructionId city{K::City,1},district{K::District,1},block{K::Complex,1},plot{K::Plot,1},court{K::Space,1},entry{K::Space,2},deck{K::Platform,1};
  p.elements={{city,city},{district,city},{block,district},{plot,block},{court,block},{entry,block},{deck,block}};
  p.elements[2].grammar=CityComplexGrammar::Hillside;
  p.elements[3].capacityArea=90;p.elements[3].requiresAccess=true;
  p.elements[4].role=CitySpaceRole::Court;p.elements[4].requiresAccess=true;
  p.elements[5].role=CitySpaceRole::Approach;p.elements[5].arrival=true;
  p.access={{1,entry,court,2},{2,court,plot,1.5},{3,court,deck,2}};
  p.supports={{plot,deck}};
  p.patches={{1,plot,{{0,0,0},{10,0,0},{10,0,9},{0,0,9}}}};
  p.demand={{101,70,plot,true,""},{102,30,{},false,"Reserved for district expansion"}};
  p.interfaces={{1,CityInterfaceKind::Entrance,plot,court,plot,7,7,{{0,0,0}},1.5,0,0}};
  return p;
}
int main() {
  int failures=0;auto check=[&](bool b,const char *s){std::cout<<(b?"PASS ":"FAIL ")<<s<<'\n';failures+=!b;};
  std::vector<CityConstructionIssue> issues;
  auto valid=fixture();check(validateCityConstruction(valid,issues),"connected hillside complex with explicit unmet demand is valid");
  auto rejects=[&](CityConstructionPlan p,const char *code){validateCityConstruction(p,issues);return std::any_of(issues.begin(),issues.end(),[&](const auto &i){return i.code==code;});};
  auto p=valid;p.elements.push_back(p.elements.back());check(rejects(p,"duplicate_or_zero_identity"),"duplicate identities rejected across one domain");
  p=valid;p.elements[1].owner=p.elements[2].id;check(rejects(p,"ownership_cycle"),"ownership cycle rejected");
  p=valid;p.elements[3].owner={K::Complex,99};check(rejects(p,"missing_owner"),"missing owner rejected");
  p=valid;p.access.erase(p.access.begin());check(rejects(p,"unreachable_required_space"),"disconnected required spaces rejected");
  p=valid;p.supports.push_back({p.elements[6].id,p.elements[3].id});check(rejects(p,"support_cycle"),"cyclic supports rejected");
  p=valid;p.supports.clear();check(rejects(p,"unused_platform"),"decorative platform without purpose rejected");
  p=valid;p.access.pop_back();check(rejects(p,"unreachable_required_space"),"inaccessible platform rejected");
  p=valid;p.patches.push_back(p.patches.front());check(rejects(p,"duplicate_ground_patch"),"ground patch cannot have two owners");
  p=valid;p.demand.back().unmetReason.clear();check(rejects(p,"unaccounted_programme"),"unplaced historical demand requires reason");
  p=valid;p.demand.front().requiredArea=91;check(rejects(p,"programme_capacity_exceeded"),"historical area cannot exceed allocated capacity");
  p=valid;p.demand.push_back(p.demand.front());check(rejects(p,"duplicate_historical_request"),"history cannot be allocated twice");
  p=valid;p.interfaces[0].acceptedB=6;check(rejects(p,"unaccepted_interface_revision"),"both sides must accept current revision");
  p=valid;p.interfaces[0].owner={K::City,1};check(rejects(p,"invalid_interface_ownership"),"shared interface has one incident owner");
  p=valid;p.elements[4].role=CitySpaceRole::None;check(rejects(p,"missing_connected_grammar_space"),"grammar requires connected court");
  for(auto grammar:{CityComplexGrammar::Castle,CityComplexGrammar::Gatehouse,CityComplexGrammar::Market,CityComplexGrammar::Quay,CityComplexGrammar::Bridge}) {
    p=valid;p.elements[2].grammar=grammar;
    check(rejects(p,"missing_connected_grammar_space"),"complex grammar rejects missing required spaces");
  }
  for(auto grammar:{CityComplexGrammar::Castle,CityComplexGrammar::Gatehouse,CityComplexGrammar::Market,CityComplexGrammar::Quay,CityComplexGrammar::Bridge}) {
    p=valid;p.elements[2].grammar=grammar;
    for(auto role:cityComplexRule(grammar).requiredSpaces) {
      CityConstructionElement e;e.id={K::Space,100+uint64_t(role)};e.owner=p.elements[2].id;
      e.role=role;e.requiresAccess=true;p.elements.push_back(e);
      p.access.push_back({100+uint64_t(role),p.elements[5].id,e.id,2});
    }
    if(grammar==CityComplexGrammar::Bridge)p.supports.push_back({p.elements[2].id,p.elements[6].id});
    check(validateCityConstruction(p,issues),"complete grammar with connected spaces and supports accepts");
  }
  p=valid;p.elements[3].owner=p.elements[1].id;check(rejects(p,"invalid_hierarchy"),"plots require a complex owner");
  p=valid;p.elements[2].grammar=CityComplexGrammar::Bridge;check(rejects(p,"unsupported_complex"),"bridge requires structural support");
  p=valid;p.access[0].width=NAN;check(rejects(p,"invalid_access_width"),"nonfinite dimensions rejected");
  p=valid;p.patches[0].boundary.resize(2);check(rejects(p,"degenerate_ground_patch"),"degenerate patch rejected");
  CityConstructionPlan published;check(publishCityConstruction(valid,published,issues),"valid blueprint publishes");
  p=valid;p.revision=8;check(!publishCityConstruction(p,published,issues)&&published.revision==7,"failed publication preserves accepted revision");
  p=valid;std::reverse(p.elements.begin(),p.elements.end());check(validateCityConstruction(p,issues),"identity and validation independent of element order");
  CityCompletionPlan legacy;legacy.revision=1;
  CitySkeletonRegion region;region.id=9;region.kind=CityRegionKind::Lot;region.boundary=valid.patches[0].boundary;
  legacy.skeleton.regions={region};legacy.historicalLots={{101,9},{102,0}};legacy.lots={{9,true}};
  auto candidate=cityConstructionCandidate(legacy);
  check(candidate.demand.size()==2 && candidate.demand[0].placed && !candidate.demand[1].placed,"pipeline adapter retains allocated and unmet history");
  check(!candidate.issues.empty() && rejects(candidate,"invalid_programme_area"),"legacy missing area contract stays explicitly unresolved");
  legacy.historicalLots[0].requiredFootprintArea=60;legacy.historicalLots[1].requiredFootprintArea=45;
  candidate=cityConstructionCandidate(legacy);
  check(!rejects(candidate,"invalid_programme_area")&&candidate.demand[1].requiredArea==45,
        "pipeline adapter preserves programme area even for unallocated demand");
  CityPublicSpace court;court.owner=9;court.kind=CityPublicSpace::Kind::Court;court.boundary=region.boundary;
  legacy.skeleton.publicSpaces={court};candidate=cityConstructionCandidate(legacy);
  const auto spaceId=candidate.elements.back().id;
  std::reverse(legacy.skeleton.publicSpaces[0].boundary.begin(),legacy.skeleton.publicSpaces[0].boundary.end());
  check(cityConstructionCandidate(legacy).elements.back().id==spaceId,"public space identity survives ring reversal");
  CityReliefPlan relief;CityReliefPlan::RaisedBlock deck;deck.block=9;deck.boundary=region.boundary;deck.level=3;
  relief.platforms={deck};candidate=cityConstructionCandidate(legacy,&relief);
  check(candidate.volumes.size()==1 && candidate.volumes[0].top==3 && rejects(candidate,"unused_platform"),
        "existing platform volume is retained and unsupported purpose is diagnosed");
  p=valid;p.volumes={{1,p.elements[6].id,CityConstructionVolume::Use::Support,valid.patches[0].boundary,4,2}};
  check(rejects(p,"invalid_volume"),"inverted structural volume rejected");
  std::cout<<"CITY_CONSTRUCTION_TESTS failures="<<failures<<'\n';return failures?1:0;
}
