#include "WorldStoryGraph.hpp"
#include "WorldKingdomHistory.hpp"
#include <algorithm>
#include <chrono>
#include <random>

namespace procedural::world::rewrite {
WorldStoryGraph::WorldStoryGraph() {
  for(const auto *name:{"lives in","belongs to","lives at","works at","parent of","is alive","stands in","household home","event at","event subject","caused by","connected to","needs food","governs","partner of"}) {
    auto id=types.intern(name,2);types.mutableType(id).usedForward=true;
  }
  edges.configure(types);edges.reserveVertices(100000);edges.setVertexCeiling(0x80000000u);
}
void WorldStoryGraph::connect(const char *name,uint32_t a,uint32_t b) {
  graphrewrite::compiler::EdgeTypeId type; if(types.find(name,type)&&!edges.containsEdge(type,{a,b}))edges.addEdge(type,{a,b});
}
void WorldStoryGraph::disconnect(const char *name,uint32_t a,uint32_t b) {
  graphrewrite::compiler::EdgeTypeId type;if(types.find(name,type))edges.removeEdge(type,{a,b});
}
bool generateWorldStory(const KingdomPlan &plan,const KingdomHistorySettings &s,KingdomHistoryResult &out,
                        std::shared_ptr<const WorldStoryGraph> &output,std::string *error) {
  const auto started=std::chrono::steady_clock::now();
  out={};output.reset();
  auto fail=[&](const std::string &why){out.error=why;if(error)*error=why;return false;};
  if(plan.settlements.empty()||!s.years||s.years>2000)return fail("world story needs settlements and 1..2000 years");
  uint64_t initial=0;
  for(const auto &site:plan.settlements)initial+=kingdomFoundingPopulation(site.role,site.population);
  if(initial>s.maximumLiving)return fail("founding population exceeds maximumLiving; raise the limit or request fewer settlements");
  auto g=std::make_shared<WorldStoryGraph>();std::mt19937 rng(s.seed);
  auto chance=[&](double p){return std::generate_canonical<double,32>(rng)<p;};
  auto &householdSite=g->households;
  std::map<uint32_t,uint32_t> siteClass; // Settlement node -> SettlementClass.
  std::map<uint32_t,uint32_t> homeOf,workOf;
  std::map<uint32_t,CityOccupation> tradeOf;
  std::map<uint32_t,uint32_t> lastCrisis,rulers,deathEvents;
  std::map<uint64_t,uint32_t> nodes;
  const CityOccupation crafts[]={CityOccupation::Carpenter,CityOccupation::Smith,CityOccupation::Baker,CityOccupation::Weaver,
    CityOccupation::Mason,CityOccupation::Merchant,CityOccupation::Innkeeper,CityOccupation::Brewer,CityOccupation::Potter,
    CityOccupation::Herbalist,CityOccupation::Scribe};
  auto event=[&](uint32_t year,uint32_t site,uint32_t subject,const std::string &kind,const std::string &text,bool assumed=false,uint32_t cause=0){
    const auto id=g->edges.allocateVertex();g->stories.push_back({id,year,site,subject,cause,kind,text,assumed});
    g->connect("event at",id,site);if(subject)g->connect("event subject",id,subject);if(cause)g->connect("caused by",id,cause);return id;
  };
  auto building=[&](uint32_t site,uint32_t house,CityBuildingUse use,CityOccupation trade,uint32_t year){
    const auto id=g->edges.allocateVertex();g->buildings[id]={id,site,house,year,use,trade,use==CityBuildingUse::Home?2u:1u};
    g->connect("stands in",id,site);return id;
  };
  auto household=[&](uint32_t site,uint32_t year,uint32_t ordinal,uint32_t role){
    const auto id=g->edges.allocateVertex();householdSite[id]=site;
    const auto trade=role>0 && ordinal%3==2?crafts[(ordinal/3)%11]:CityOccupation::Farmer;tradeOf[id]=trade;
    homeOf[id]=building(site,id,CityBuildingUse::Home,trade,year);g->connect("household home",id,homeOf[id]);
    // A town's farmers walk out to the fields: no farmstead inside the walls.
    // A second building per household asked a capital for 4,941 buildings
    // from 2,479 households, a quarter more than its lots.
    const bool urban=siteClass.count(site) && siteClass.at(site)>=uint32_t(SettlementClass::Town);
    workOf[id]=trade==CityOccupation::Farmer && urban ? homeOf[id]
        : building(site,id,trade==CityOccupation::Farmer?CityBuildingUse::Farm:CityBuildingUse::Workshop,trade,year);
    return id;
  };
  auto person=[&](uint32_t site,uint32_t house,uint32_t born){
    const auto id=g->edges.allocateVertex();g->people[id]={id,born,0,site,house,homeOf[house],0,tradeOf[house]};
    g->connect("lives in",id,site);g->connect("belongs to",id,house);g->connect("lives at",id,homeOf[house]);g->connect("is alive",id,id);return id;
  };
  uint32_t living=uint32_t(initial);
  for(size_t i=0;i<plan.settlements.size();++i) {
    if(s.cancelled&&s.cancelled())return fail("world history cancelled");
    const auto &site=plan.settlements[i];const auto node=kingdomHistoryNode(i);nodes[site.id]=node;g->settlements[node]=site.id;
    const auto count=kingdomFoundingPopulation(site.role,site.population);out.foundingPopulations[node]=count;
    siteClass[node]=uint32_t(site.role);
    const auto ancient=500+(s.seed+uint32_t(site.id)*31)%350;
    event(ancient,node,node,"legendary founding","Families reclaimed an ancient sanctuary and the fields around it.",true);
    event(ancient+80,node,node,"land charter","An inherited charter established farms, household plots and craft rights.",true);
    uint32_t remaining=count,ordinal=0;
    while(remaining) {
      const auto members=std::min(4u,remaining);const auto h=household(node,980+rng()%20,ordinal++,uint32_t(site.role));
      uint32_t parent=0;
      for(uint32_t p=0;p<members;++p){
        const uint32_t age=p<2?24+rng()%25:rng()%std::min(18u,1000-g->people.at(parent).bornYear-17);
        const auto id=person(node,h,1000-age);if(p==0)parent=id;else if(p>=2)g->connect("parent of",parent,id);
      }
      remaining-=members;
    }
    building(node,0,CityBuildingUse::Church,CityOccupation::Cleric,ancient+80);
    if(count>=500)building(node,0,CityBuildingUse::Market,CityOccupation::Merchant,960);
    // A seat large enough for a keep also has its lord's manor.
    if(count>1000)building(node,0,CityBuildingUse::Keep,CityOccupation::Guard,930);
    if(count>=100)building(node,0,CityBuildingUse::Manor,CityOccupation::Administrator,970);
  }
  for(const auto &link:plan.links)if(nodes.count(link.from)&&nodes.count(link.to)){
    g->connect("connected to",nodes[link.from],nodes[link.to]);g->connect("connected to",nodes[link.to],nodes[link.from]);
  }
  // Only current membership is indexed transiently. Story and ancestry facts
  // accumulate in the graph; no population copies are retained per year.
  for(uint32_t elapsed=0;elapsed<=s.years;++elapsed) {
    if(s.cancelled&&s.cancelled())return fail("world history cancelled");
    g->year=1000+elapsed;
    std::map<uint32_t,std::vector<uint32_t>> members;
    std::vector<uint32_t> births;
    for(auto &[id,p]:g->people)if(!p.diedYear) {
      const uint32_t age=g->year-p.bornYear;
      if(elapsed && chance((age<5?.006:age<60?.004:std::min(.6,.04+(age-60)*.012)) * (lastCrisis.count(p.settlement)?1.25:1.))) {
        p.diedYear=g->year;--living;g->disconnect("is alive",id,id);g->disconnect("lives in",id,p.settlement);
        g->disconnect("lives at",id,p.home);if(p.workplace)g->disconnect("works at",id,p.workplace);p.workplace=0;
        deathEvents[id]=event(g->year,p.settlement,id,"death","A household lost a member.");continue;
      }
      if(age>=16 && age<70 && !p.workplace){p.workplace=workOf[p.household];g->connect("works at",id,p.workplace);}
      if(age>=70 && p.workplace){g->disconnect("works at",id,p.workplace);p.workplace=0;}
      members[p.household].push_back(id);
    }
    if(elapsed)for(const auto &[house,family]:members) {
      auto parent=std::find_if(family.begin(),family.end(),[&](uint32_t id){auto age=g->year-g->people.at(id).bornYear;return age>=20&&age<42;});
      if(parent!=family.end()&&family.size()<7&&chance(.22))births.push_back(*parent);
    }
    // Allocate scarce population capacity across the whole world, not to
    // whichever settlement happens to have the lowest graph node IDs.
    const auto available=s.maximumLiving>living?s.maximumLiving-living:0u;
    if(births.size()>available){std::shuffle(births.begin(),births.end(),rng);births.resize(available);}
    for(auto parent:births){const auto &p=g->people.at(parent);const auto id=person(p.settlement,p.household,g->year);g->connect("parent of",parent,id);++living;
      event(g->year,p.settlement,id,"birth","A new generation was born.");}
    // Young adults from separate households form a shared home, rather than
    // turning every departing child into another one-person house/workshop.
    std::map<uint32_t,std::vector<uint32_t>> partners;
    if(elapsed)for(const auto &[house,family]:members)if(family.size()>4) {
      for(size_t i=2;i<family.size();++i){const auto &p=g->people.at(family[i]);const auto age=g->year-p.bornYear;
        if(age>=20&&age<40&&chance(.15))partners[p.settlement].push_back(p.id);}
    }
    for(const auto &[site,candidates]:partners)for(size_t i=0;i+1<candidates.size();i+=2) {
      auto &a=g->people.at(candidates[i]);auto &b=g->people.at(candidates[i+1]);
      if(a.household==b.household)continue;
      uint32_t h=0;
      for(const auto &[vacant,location]:householdSite)if(location==site&&!members.count(vacant)){h=vacant;break;}
      if(!h)h=household(site,g->year,uint32_t(householdSite.size()),1);
      members[h]={a.id,b.id};
      for(auto id:{a.id,b.id}){
        auto &p=g->people.at(id);
        g->disconnect("belongs to",id,p.household);g->disconnect("lives at",id,p.home);if(p.workplace)g->disconnect("works at",id,p.workplace);
        p.household=h;p.home=homeOf[h];p.workplace=workOf[h];p.occupation=tradeOf[h];
        g->connect("belongs to",id,h);g->connect("lives at",id,p.home);g->connect("works at",id,p.workplace);
      }
      g->connect("partner of",a.id,b.id);g->connect("partner of",b.id,a.id);
      event(g->year,site,h,"new household","Two adults joined households and established a home and trade.");
    }
    if(elapsed)for(size_t i=0;i<plan.settlements.size();++i) {
      const auto node=kingdomHistoryNode(i);
      if(lastCrisis.count(node) && elapsed%7==0){
        const auto b=building(node,0,CityBuildingUse::Market,CityOccupation::Merchant,g->year);
        event(g->year,node,b,"recovery","Neighbours rebuilt a market to restore trade after hardship.",false,lastCrisis[node]);lastCrisis.erase(node);
      } else if(chance(.025))lastCrisis[node]=event(g->year,node,node,"poor harvest","A poor harvest brought hardship and calls for communal rebuilding.");
    }
    for(const auto &[site,identity]:g->settlements) {
      const auto previous=rulers[site];
      if(previous&&!g->people.at(previous).diedYear)continue;
      uint32_t successor=0;
      for(const auto &[id,p]:g->people)if(!p.diedYear&&p.settlement==site&&g->year-p.bornYear>=18&&g->year-p.bornYear<65) {
        if(!successor||p.bornYear<g->people.at(successor).bornYear)successor=id;
      }
      if(previous)g->disconnect("governs",previous,site);
      rulers[site]=successor;
      if(successor){g->connect("governs",successor,site);event(g->year,site,successor,"succession",
        previous?"A new leader took responsibility after the former leader died.":"The households recognised a local leader.",false,previous?deathEvents[previous]:0);}
    }
    // Fill civic roles and express current food demand as graph facts.
    // These are qualitative rules: no currency, inventories or ledgers.
    std::set<uint32_t> staffed;
    for(const auto &[id,p]:g->people)if(!p.diedYear&&p.workplace)staffed.insert(p.workplace);
    std::set<uint32_t> reassigned;
    for(const auto &[id,b]:g->buildings)if(!b.household&&!staffed.count(id)) {
      for(auto &[pid,p]:g->people)if(!p.diedYear&&p.settlement==b.settlement&&p.workplace&&g->year-p.bornYear<60&&!reassigned.count(pid)) {
        const auto old=g->buildings.at(p.workplace);
        if(!old.household)continue;
        g->disconnect("works at",pid,p.workplace);p.workplace=id;p.occupation=b.occupation;g->connect("works at",pid,id);reassigned.insert(pid);break;
      }
    }
    std::map<uint32_t,uint32_t> needs;
    for(const auto &[id,p]:g->people)if(!p.diedYear)needs[p.settlement]+=4;
    for(const auto &[site,identity]:g->settlements) {
      if(g->foodNeeds.count(site))g->disconnect("needs food",site,0x80000000u+g->foodNeeds[site]);
      g->foodNeeds[site]=needs[site];g->connect("needs food",site,0x80000000u+needs[site]);
    }
    if(elapsed&&s.progress)s.progress(elapsed);
  }
  for(size_t i=0;i<plan.settlements.size();++i) {
    KingdomHistorySettlement one;one.settlement=plan.settlements[i].id;one.role=plan.settlements[i].role;one.node=kingdomHistoryNode(i);
    one.brief.settlement=one.node;one.brief.week=g->year*52;
    std::map<uint32_t,SettlementHousehold> families;
    std::map<uint32_t,uint32_t> residents,workers;
    for(const auto &[id,p]:g->people)if(!p.diedYear&&p.settlement==one.node){
      auto &h=families[p.household];h.id=p.household;++h.members;h.workers+=p.workplace!=0;++residents[p.home];if(p.workplace)++workers[p.workplace];
      HistoricalCitizenBrief citizen;citizen.id=id;citizen.household=p.household;citizen.home=p.home;
      if(p.workplace){citizen.workplace=p.workplace;citizen.workBuilding=p.workplace;citizen.occupation=p.occupation;}
      one.brief.citizens.push_back(citizen);
    }
    for(const auto &[id,h]:families)one.households.push_back(h);
    one.brief.households=one.households;
    if(!deriveCityPopulationDemand(one.households,{},one.brief.demand,error))return false;
    for(const auto &[id,b]:g->buildings)if(b.settlement==one.node){
      if(b.household && (b.use==CityBuildingUse::Home ? residents[id]==0 : workers[id]==0))continue;
      HistoricalBuildingBrief record;record.id=id;record.use=b.use;record.builtWeek=b.builtYear*52;record.floors=b.floors;record.residents=residents[id];record.workers=workers[id];
      if(b.use==CityBuildingUse::Home)record.household=b.household;
      else {record.workplace=id;record.trade=historicalTradeOf(b.occupation);}
      one.brief.buildings.push_back(record);
      one.buildings.push_back({id,cityBuildingUseName(b.use),record.builtWeek,b.floors,record.residents,record.trade,g->year-b.builtYear});
      one.castle|=b.use==CityBuildingUse::Keep;one.manor|=b.use==CityBuildingUse::Manor;
    }
    out.report.living+=one.brief.demand.population;out.report.buildings+=uint32_t(one.buildings.size());out.report.households+=uint32_t(one.households.size());
    out.settlements.push_back(std::move(one));
  }
  out.report.startingLiving=uint32_t(initial);out.report.totalSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  output=std::move(g);return true;
}
} // namespace procedural::world::rewrite
