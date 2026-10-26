#pragma once
#include "procedural/world/WorldStoryGraph.hpp"
#include "procedural/world/WorldKingdomHistory.hpp"
#include "json.hpp"
inline nlohmann::json worldStoryJson(const procedural::world::rewrite::WorldStoryGraph &g) {
  using nlohmann::json;
  json out={{"schemaVersion",1},{"year",g.year},{"valueBase",0x80000000u},{"nodes",json::array()},{"edges",json::array()},{"stories",json::array()}};
  for(const auto &[id,worldId]:g.settlements)out["nodes"].push_back({{"id",id},{"kind","settlement"},{"worldId",worldId},{"monthlyFoodNeed",g.foodNeeds.at(id)}});
  for(const auto &[id,site]:g.households)out["nodes"].push_back({{"id",id},{"kind","household"},{"settlement",site}});
  for(const auto &[id,p]:g.people)out["nodes"].push_back({{"id",id},{"kind","person"},{"bornYear",p.bornYear},{"diedYear",p.diedYear},{"settlement",p.settlement},{"household",p.household},{"occupation",procedural::world::rewrite::cityOccupationName(p.occupation)}});
  for(const auto &[id,b]:g.buildings)out["nodes"].push_back({{"id",id},{"kind","building"},{"builtYear",b.builtYear},{"use",procedural::world::rewrite::cityBuildingUseName(b.use)},{"occupation",procedural::world::rewrite::cityOccupationName(b.occupation)},{"settlement",b.settlement}});
  for(uint32_t i=0;i<g.edges.partitionCount();++i){const auto &part=g.edges.partitionAt(i);for(const auto &page:part.allPages())for(const auto &edge:page)
    out["edges"].push_back({g.types[part.edgeType()].pattern,edge.origin,edge.target});}
  for(const auto &e:g.stories)out["stories"].push_back({{"id",e.id},{"year",e.year},{"settlement",e.settlement},{"subject",e.subject},{"cause",e.cause},{"kind",e.kind},{"narrative",e.narrative},{"assumed",e.assumed}});
  return out;
}
inline nlohmann::json worldStorySettlementJson(const procedural::world::rewrite::WorldCivilization &c,const procedural::world::rewrite::KingdomHistorySettlement &s) {
  using nlohmann::json;using namespace procedural::world::rewrite;
  json out={{"schemaVersion",2},{"socialSource","annual-world-story-graph"},{"year",c.story->year-1000},{"week",c.story->year*52},
    {"settlement",s.node},{"population",s.brief.demand.population},{"foundingPopulation",c.history.foundingPopulations.at(s.node)},
    {"households",s.households.size()},{"workers",s.brief.demand.workers},{"buildings",json::array()},{"citizens",json::array()},
    {"householdRecords",json::array()},{"stories",json::array()},{"graphFile","story-graph.json"}};
  for(const auto &h:s.households)out["householdRecords"].push_back({{"id",h.id},{"members",h.members},{"workers",h.workers},{"collective",h.collective}});
  for(const auto &b:s.brief.buildings)out["buildings"].push_back({{"id",b.id},{"use",cityBuildingUseName(b.use)},{"builtWeek",b.builtWeek},{"floors",b.floors},{"residents",b.residents},{"workers",b.workers},{"trade",b.trade}});
  for(const auto &p:s.brief.citizens)out["citizens"].push_back({{"id",p.id},{"household",p.household},{"home",p.home?json(*p.home):json(nullptr)},
    {"workplace",p.workplace?json(*p.workplace):json(nullptr)},{"workBuilding",p.workBuilding?json(*p.workBuilding):json(nullptr)},
    {"occupation",p.occupation?json(cityOccupationName(*p.occupation)):json(nullptr)}});
  for(const auto &e:c.story->stories)if(e.settlement==s.node)out["stories"].push_back({{"id",e.id},{"year",e.year},{"kind",e.kind},{"subject",e.subject},{"cause",e.cause},{"narrative",e.narrative},{"assumed",e.assumed}});
  return out;
}
