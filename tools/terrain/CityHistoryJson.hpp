// The dated history of one settlement as JSON: residents, households,
// buildings with their trades, ancestry, residence and migration events and
// the funded defence circuits. Shared by the timeline preview and the world
// preview, so a kingdom's settlements and a scale run read the same way.
#pragma once
#include "json.hpp"
#include "procedural/world/WorldCityChronicle.hpp"
#include "procedural/world/WorldCityHistoricalBrief.hpp"
#include <string>

inline nlohmann::json historyJson(const procedural::world::rewrite::CityState &state, const procedural::world::rewrite::HistoryNode settlement,
                                  const procedural::world::rewrite::HistoricalCityBrief &brief, const procedural::world::rewrite::CityFabricSite &site,
                                  const char *socialSource, const uint32_t year)
{
  using namespace procedural::world::rewrite;
  using json = nlohmann::json;
  auto optional = [](auto v) -> json {
    return v ? json(*v) : json(nullptr);
  };
        json j = {
          {"schemaVersion", 2},
          {"socialSource", socialSource},
          {"year", year},
          {"week", brief.week},
          {"settlement", settlement},
          {"population", brief.demand.population},
          {"households", brief.demand.households},
          {"workers", brief.demand.workers},
          {"programmeLandArea", brief.demand.programmeLandArea},
          {"circulationArea", brief.demand.circulationArea},
          {"waitingHouseholds", brief.waitingHouseholds},
          {"buildings", json::array()},
          {"citizens", json::array()},
          {"ancestry", json::array()},
          {"activities", json::array()}};
        j["householdRecords"] = json::array();
        for (const auto &h : brief.households)
        {
          j["householdRecords"].push_back({{"id", h.id}, {"members", h.members}, {"workers", h.workers}, {"collective", h.collective}});
        }
        for (const auto &b : brief.buildings)
        {
          j["buildings"].push_back(
              {{"id", b.id},
               {"use", cityBuildingUseName(b.use)},
               {"builtWeek", b.builtWeek},
               {"floors", b.floors},
               {"residents", b.residents},
               {"workers", b.workers},
               {"household", optional(b.household)},
               {"workplace", optional(b.workplace)},
               {"trade", b.trade}});
        }
        for (const auto &p : brief.citizens)
        {
          j["citizens"].push_back(
              {{"id", p.id},
               {"household", p.household},
               {"home", optional(p.home)},
               {"workplace", optional(p.workplace)},
               {"workBuilding", optional(p.workBuilding)},
               {"occupation", p.occupation ? json(cityOccupationName(*p.occupation)) : json(nullptr)},
               {"duty", p.duty},
               {"apprentice", p.apprentice}});
        }
        // Stable fictional names are presentation identity, never the join key.
        const char *given[]={"Elin","Rowan","Mira","Alden","Tamsin","Bram","Lyra","Gareth","Nessa","Oren","Sera","Dorian","Wren","Cedric","Iona","Torin"};
        const char *family[]={"Ashford","Reed","Hawthorn","Vale","Marsh","Oakley","Fenwick","Hale","Briar","Stone","Morrow","Wells","Thorne","Dale","Lark","Winter"};
        const auto personName=[&](uint32_t id) {
          const auto found=state.previous->people.find(id);
          const uint64_t familyId=found!=state.previous->people.end()?found->second.dynasty.value_or(id):id;
          return std::string(given[(id*2654435761u)%16])+" "+family[((familyId>>12)^familyId)%16];
        };
        for(auto &person:j["citizens"]) {
          const auto id=person["id"].get<uint32_t>();
          person["name"]=personName(id);
          person["nameSource"]="seeded fictional name";
          const auto found=state.previous->people.find(id);
          if(found!=state.previous->people.end())person["bornWeek"]=found->second.bornWeek;
          const auto career=state.economy.livelihoods().find(id);
          if(career!=state.economy.livelihoods().end()) {
            const auto &c=career->second;
            person["careerStage"]=cityCareerStageName(c.career);
            person["mentor"]=optional(c.mentor);
            person["ambition"]=c.ambition?json(cityOccupationName(*c.ambition)):json(nullptr);
          }
        }
        j["workEvents"]=json::array();
        for(const auto &e:state.economy.events())if(e.settlement && *e.settlement==settlement && e.person &&
          (e.kind==CityEconomyEventKind::Hired || e.kind==CityEconomyEventKind::SelfEmployed || e.kind==CityEconomyEventKind::Released ||
           e.kind==CityEconomyEventKind::Apprenticed || e.kind==CityEconomyEventKind::Graduated || e.kind==CityEconomyEventKind::Retired || e.kind==CityEconomyEventKind::Opened || e.kind==CityEconomyEventKind::Mastered || e.kind==CityEconomyEventKind::Ambition || e.kind==CityEconomyEventKind::OwnerChanged))
        {
          const auto place=state.economy.workplaces().find(e.workplace);
          j["workEvents"].push_back({{"week",e.week},{"kind",cityEconomyEventName(e.kind)},{"person",*e.person},{"workplace",e.workplace},
            {"occupation",place==state.economy.workplaces().end()?json(nullptr):json(cityOccupationName(place->second.occupation))}});
        }
        // Whole family tree includes dead and emigrated relatives, with graph IDs intact.
        for (const auto &[id, p] : state.previous->people)
        {
          j["ancestry"].push_back(
              {{"id", id}, {"name",personName(id)}, {"bornWeek", p.bornWeek}, {"alive", p.alive}, {"parents", p.parents}, {"spouse", optional(p.spouse)}, {"settlement", optional(p.settlement)}, {"dynasty", optional(p.dynasty)}});
        }
  j["residenceEvents"] = json::array();
  for (const auto &e : state.fabric.events())
  {
    // A citizen biography needs actual tenancy changes, not every
    // weekly retry of a funded project waiting for a vacant parcel.
    if (e.settlement == settlement && e.household &&
        (e.kind == CityFabricEventKind::TenancyAssigned ||
         e.kind == CityFabricEventKind::TenancyReleased ||
         e.kind == CityFabricEventKind::Relocated))
    {
      j["residenceEvents"].push_back({{"week", e.week}, {"kind", cityFabricEventName(e.kind)}, {"household", e.household}, {"building", e.subject}, {"detail", e.detail}});
    }
  }
  j["migrationEvents"] = json::array();
  for (const auto &e : state.social.events())
  {
    if (e.kind == CityHistoryEventKind::Arrival || e.kind == CityHistoryEventKind::Departure || e.kind == CityHistoryEventKind::Move)
    {
      j["migrationEvents"].push_back({{"week", e.week}, {"person", e.subject}, {"kind", cityHistoryEventName(e.kind)}, {"before", optional(e.before)}, {"after", optional(e.after)}});
    }
  }
  // Compact annual evidence, rather than copying every monthly transaction.
  std::map<uint32_t,json> years;
  const uint32_t epoch=brief.week-year*52;
  for(const auto &e:state.economy.events())if(e.settlement && *e.settlement==settlement && e.week>=epoch) {
    auto &record=years[(e.week-epoch)/52];
    const char *key=e.kind==CityEconomyEventKind::Hunger?"hungerEpisodes":
      e.kind==CityEconomyEventKind::UnpaidWages?"unpaidWageEpisodes":
      e.kind==CityEconomyEventKind::Opened?"businessesOpened":
      e.kind==CityEconomyEventKind::Graduated?"graduates":nullptr;
    if(key){if(record.is_null())record=json::object();record[key]=record.value(key,0u)+1;}
  }
  j["economicYears"]=json::array();for(auto &[y,r]:years)if(!r.is_null()){r["year"]=y;j["economicYears"].push_back(r);}
  std::set<HistoryNode> localPeople,localDynasties;
  for(const auto &[id,p]:state.previous->people)if(p.settlement && *p.settlement==settlement) {
    localPeople.insert(id);if(p.dynasty)localDynasties.insert(*p.dynasty);
  }
  j["socialEvents"]=json::array();
  for(const auto &e:state.social.events()) {
    const bool feud=e.kind==CityHistoryEventKind::FeudStarted || e.kind==CityHistoryEventKind::FeudEnded;
    const bool relevant=feud?(localDynasties.count(e.subject) || (e.before && localDynasties.count(*e.before)) || (e.after && localDynasties.count(*e.after))):
      e.kind==CityHistoryEventKind::Succession?true:localPeople.count(e.subject)>0;
    if(relevant && e.kind!=CityHistoryEventKind::LifeStageChange)
      j["socialEvents"].push_back({{"week",e.week},{"kind",cityHistoryEventName(e.kind)},
        {"subject",e.subject},{"before",optional(e.before)},{"after",optional(e.after)}});
  }
  auto defence = state.defence.sites().find(settlement);
  j["defenceStage"] = defence == state.defence.sites().end() ? "none" : cityDefenceStageName(defence->second.stage);
  // Semantic enclosure membership survives re-planning the physical city.
  // Export actual funded ledger records, never infer walls from city age.
  j["defenceCircuits"] = json::array();
  if (defence != state.defence.sites().end()) {
    for (const auto &c : defence->second.circuits) {
      json members = json::array(), segments = json::array(), gates = json::array(), towers = json::array();
      for (const auto &[id, building] : site.buildings) {
        const auto parcel = site.parcels.find(building.parcel);
        if (building.active(brief.week) && parcel != site.parcels.end() && c.enclosedBlocks.count(parcel->second.block))
          members.push_back(id);
      }
      for (const auto &segment : c.segments)
        segments.push_back({{"id",segment.id},{"state",cityStructureStateName(segment.state)},{"completedWeek",optional(segment.completedWeek)}});
      for (const auto &gate : c.gates)
        gates.push_back({{"id",gate.id},{"segment",gate.segment},{"route",gate.route},{"state",cityStructureStateName(gate.state)},{"completedWeek",optional(gate.completedWeek)}});
      for (const auto &tower : c.towers)
        towers.push_back({{"id",tower.id},{"segment",tower.segment},{"state",cityStructureStateName(tower.state)},{"completedWeek",optional(tower.completedWeek)}});
      j["defenceCircuits"].push_back({{"id",c.id},{"project",c.project},{"era",c.era},{"palisade",c.palisade},
        {"lifecycle",cityWallLifecycleName(c.lifecycle)},{"closed",c.closed()},{"abandoned",c.abandoned},
        {"closedWeek",optional(c.closedWeek)},{"absorbedWeek",optional(c.absorbedWeek)},
        {"enclosedBuildings",members},{"segments",segments},{"gates",gates},{"towers",towers}});
    }
  }
  return j;
}
