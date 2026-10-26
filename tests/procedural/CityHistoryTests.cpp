#include "procedural/world/WorldCityDemand.hpp"
#include "procedural/world/WorldCityHistory.hpp"
#include <iostream>
using namespace procedural::world::rewrite;
using namespace graphrewrite;

int main()
{
  int failures = 0;
  auto check = [&](bool ok, const char *text) {
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
    failures += !ok;
  };
  compiler::EdgeTypeTable types;
  for (const char *s : {"? born in ?", "? is alive", "? is dead", "? lives in ?", "? is of house ?", "? is child of ?", "? is king of ?"})
  {
    auto id = types.intern(s, std::string(s).find("?", 1) == std::string::npos ? 1 : 2);
    types.mutableType(id).usedForward = true;
  }
  runtime::EdgePageStore graph;
  graph.configure(types);
  auto change = [&](const char *s, uint32_t a, uint32_t b, bool add = true) {
    compiler::EdgeTypeId id;
    types.find(s, id);
    if (add)
    {
      graph.addEdge(id, {a, b});
    }
    else
    {
      graph.removeEdge(id, {a, b});
    }
  };
  constexpr uint32_t base = 1u << 24, now = 1000 * 52;
  auto person = [&](uint32_t id, uint32_t age) {
    change("? born in ?", id, base + now - age * 52);
    change("? is alive", id, id);
    change("? lives in ?", id, 100);
    change("? is of house ?", id, 200);
  };
  person(0, 40);
  person(1, 18);
  person(2, 5);
  change("? is child of ?", 1, 0);
  change("? is child of ?", 2, 0);
  change("? is king of ?", 0, 300);
  CityHistorySnapshot first;
  std::string error;
  check(readCityHistorySnapshot(types, graph, now, base, first, &error), "valid graph projects into history");
  check(first.rulers.at(300) == 0 && first.people.at(1).parents == std::vector<uint32_t>{0}, "node zero remains valid for ruler and parent");
  auto demand = cityHistoryDemand(first, 100);
  check(demand.residents == 3 && demand.children == 1 && demand.workingAge == 2 && demand.familyCohorts.size() == 1, "demand counts living residents and adult capacity separately");
  CityHistoryLedger ledger;
  check(ledger.observe(first, &error) && ledger.events().empty(), "founding cast creates no false births");
  change("? is alive", 0, 0, false);
  change("? is dead", 0, 0);
  change("? is king of ?", 0, 300, false);
  change("? is king of ?", 1, 300);
  change("? lives in ?", 1, 100, false);
  change("? lives in ?", 1, 101);
  change("? born in ?", 3, base + now + 1);
  change("? is alive", 3, 3);
  change("? lives in ?", 3, 100);
  change("? is child of ?", 3, 1);
  CityHistorySnapshot second;
  check(readCityHistorySnapshot(types, graph, now + 1, base, second, &error) && ledger.observe(second, &error), "death, migration, succession and newborn are accepted");
  int births = 0, deaths = 0, moves = 0, succession = 0;
  for (auto e : ledger.events())
  {
    births += e.kind == CityHistoryEventKind::Birth;
    deaths += e.kind == CityHistoryEventKind::Death;
    moves += e.kind == CityHistoryEventKind::Move;
    succession += e.kind == CityHistoryEventKind::Succession;
  }
  check(births == 1 && deaths == 1 && moves == 1 && succession == 1, "weekly ledger distinguishes birth, death, move and succession");
  check(second.people.count(0) && !second.people.at(0).alive && cityHistoryDemand(second, 100).residents == 2, "dead ancestors retained without consuming resident capacity");
  const auto count = ledger.events().size();
  auto invalid = second;
  invalid.week += 2;
  check(!ledger.observe(invalid, &error) && ledger.events().size() == count, "missing weeks rejected without partial events");
  invalid = second;
  ++invalid.week;
  invalid.people.erase(0);
  check(!ledger.observe(invalid, &error) && ledger.current()->week == second.week, "ancestor deletion rejected transactionally");
  invalid = second;
  ++invalid.week;
  invalid.people[0].alive = true;
  check(!ledger.observe(invalid, &error), "resurrection rejected");
  change("? lives in ?", 1, 102);
  check(!readCityHistorySnapshot(types, graph, now + 2, base, second, &error) && second.week == now + 1, "conflicting residences rejected without changing snapshot");
  change("? lives in ?", 1, 102, false);
  change("? is child of ?", 0, 2);
  check(!readCityHistorySnapshot(types, graph, now + 2, base, second, &error), "cyclic or temporally impossible ancestry rejected");
  change("? is child of ?", 0, 2, false);
  CityHistorySnapshot repeat;
  readCityHistorySnapshot(types, graph, now + 1, base, repeat, &error);
  CityHistoryLedger replay;
  replay.observe(first);
  replay.observe(repeat);
  bool same = replay.events().size() == ledger.events().size();
  for (size_t i = 0; same && i < replay.events().size(); ++i)
  {
    auto a = replay.events()[i], b = ledger.events()[i];
    same = a.week == b.week && a.kind == b.kind && a.subject == b.subject && a.before == b.before && a.after == b.after;
  }
  check(same, "identical graph observations produce identical ordered events");
  compiler::EdgeTypeTable backwardTypes = types;
  for (uint32_t i = 0; i < types.size(); ++i)
  {
    backwardTypes.mutableType(i).usedForward = false;
    backwardTypes.mutableType(i).usedBackward = true;
  }
  runtime::EdgePageStore backward;
  backward.configure(backwardTypes);
  for (uint32_t i = 0; i < types.size(); ++i)
  {
    for (auto e : graph.partition(i, runtime::KeyOrder::ByOrigin).snapshot())
    {
      backward.addEdge(i, e);
    }
  }
  CityHistorySnapshot fromBackward;
  check(
      readCityHistorySnapshot(backwardTypes, backward, now + 1, base, fromBackward, &error) && fromBackward.rulers == repeat.rulers && fromBackward.people.at(3).parents == repeat.people.at(3).parents,
      "backward-only graph indexes preserve history");
  compiler::EdgeTypeTable missingTypes;
  runtime::EdgePageStore missingGraph;
  missingGraph.configure(missingTypes);
  check(!readCityHistorySnapshot(missingTypes, missingGraph, now, base, fromBackward, &error) && fromBackward.week == now + 1, "missing required schema fails without erasing output");
  auto third = repeat;
  ++third.week;
  third.people.at(1).settlement.reset();
  check(replay.observe(third, &error) && replay.events().back().kind == CityHistoryEventKind::Departure, "emigration preserves identity and records departure");
  auto fourth = third;
  ++fourth.week;
  fourth.people.at(1).settlement = 100;
  check(replay.observe(fourth, &error) && replay.events().back().kind == CityHistoryEventKind::Arrival && fourth.people.at(1).parents == std::vector<uint32_t>{0}, "return migration retains genealogy");
  change("? is king of ?", 2, 300);
  check(!readCityHistorySnapshot(types, graph, now + 1, base, repeat, &error), "two living rulers of one realm are rejected");
  change("? is king of ?", 2, 300, false);
  change("? born in ?", 4, base + now - 12 * 52);
  change("? is alive", 4, 4);
  change("? lives in ?", 4, 100);
  change("? born in ?", 5, base + now - 65 * 52);
  change("? is alive", 5, 5);
  change("? lives in ?", 5, 100);
  check(
      readCityHistorySnapshot(types, graph, now + 1, base, repeat, &error) && cityHistoryDemand(repeat, 100).apprenticeAge == 1 && cityHistoryDemand(repeat, 100).elders == 1 &&
          cityHistoryDemand(repeat, 100).workingAge == 0,
      "apprentice-age children and elders are not silently assigned adult jobs");
  CityHistoryDemand large;
  large.familyCohorts = {{1, 19, 11, false}, {uint64_t(1) << 33, 9, 3, false}};
  auto housing = cityHistoryHousingEstimates(large);
  CityPopulationDemand land;
  check(
      deriveCityPopulationDemand(housing, {}, land, &error) && land.population == 28 && land.workers == 14 && housing.size() == 5,
      "large dynasties yield bounded housing estimates without losing population or colliding IDs");
  {
    CityHistoryLedger feuds;
    auto a=first;check(feuds.observe(a,&error),"feud fixture begins at a validated snapshot");
    ++a.week;a.feuds={{200,201},{201,200}};
    check(feuds.observe(a,&error) && feuds.events().size()==1 && feuds.events().back().kind==CityHistoryEventKind::FeudStarted,
      "symmetric feud edges emit one start event");
    ++a.week;a.feuds.clear();
    check(feuds.observe(a,&error) && feuds.events().size()==2 && feuds.events().back().kind==CityHistoryEventKind::FeudEnded,
      "feud removal records its actual ending");
  }
  return failures ? 1 : 0;
}
