#pragma once

// Shared synthetic story graph for the city history suites: a schema with the
// edges the story declares, and both phase 1 and phase 2 ledgers advanced one
// week at a time with every gate applied on each step. Later suites add their
// own ledgers on top of `advance()`.
#include "graphrewrite/runtime/NameGenerator.hpp"
#include "procedural/world/WorldCityHouseholds.hpp"
#include <string>

namespace cityfixture {
using namespace procedural::world::rewrite;
using namespace graphrewrite;

constexpr uint32_t base = 1u << 24, start = 1000 * 52;

const char *const patterns[] = {"? is alive",        "? is dead",         "? is man",          "? is woman",        "? born in ?",       "? is young",      "? is an infant",
                                "? is grown",        "? is unmarried",    "? is unattached",   "? is fertile",      "? is husband of ?", "? is wife of ?",  "? is child of ?",
                                "? is mother of ?",  "? is father of ?",  "? is of house ?",   "? is head of ?",    "? is at peace with ?", "? lives in ?", "? keeps the peace",
                                "? is king of ?",     "? serves ?",        "? is heir to ?",    "? is queen of ?",   "? claims ?",        "? wants a king",  "? swears to ?",
                                "? feuds with ?"};

// A synthetic story graph and both ledgers, advanced one week at a time with
// every phase 1 and phase 2 check applied on each step.
struct Fixture
{
  compiler::EdgeTypeTable types;
  runtime::EdgePageStore graph;
  runtime::NameTable names{7};
  uint32_t week = start;
  CityHistoryLedger history;
  CityHouseholdLedger homes;
  std::optional<CityHistorySnapshot> snapshot;
  std::string error;

  explicit Fixture(CityHouseholdPolicy policy) : homes(std::move(policy))
  {
    for (const char *s : patterns)
    {
      const auto id = types.intern(s, std::string(s).find('?', 1) == std::string::npos ? 1 : 2);

      types.mutableType(id).usedForward = true;
    }

    graph.configure(types);
  }

  void edge(const char *s, uint32_t a, uint32_t b, bool add = true)
  {
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
  }

  void person(uint32_t id, uint32_t age, uint32_t settlement, std::optional<uint32_t> house, bool man)
  {
    edge("? born in ?", id, base + week - age * 52);
    edge("? is alive", id, id);
    edge(man ? "? is man" : "? is woman", id, id);
    edge("? lives in ?", id, settlement);

    if (house)
    {
      edge("? is of house ?", id, *house);
    }

    if (age >= 16)
    {
      edge("? is grown", id, id);
      edge("? is unmarried", id, id);
      edge("? is unattached", id, id);
      edge("? keeps the peace", id, id);
    }
    else
    {
      edge("? is young", id, id);
    }
  }

  void child(uint32_t id, uint32_t father, uint32_t mother)
  {
    edge("? is child of ?", id, father);
    edge("? is child of ?", id, mother);
    edge("? is father of ?", father, id);
    edge("? is mother of ?", mother, id);
  }

  void marry(uint32_t man, uint32_t woman)
  {
    edge("? is husband of ?", man, woman);
    edge("? is wife of ?", woman, man);
    edge("? is unmarried", man, man, false);
    edge("? is unmarried", woman, woman, false);
  }

  void kill(uint32_t id)
  {
    edge("? is alive", id, id, false);
    edge("? is dead", id, id);
  }

  void moveTo(uint32_t id, uint32_t from, uint32_t to)
  {
    edge("? lives in ?", id, from, false);
    edge("? lives in ?", id, to);
  }

  // The weekly contract: graph -> social snapshot -> social ledger ->
  // reconciliation -> household ledger -> validation.
  bool advance()
  {
    if (snapshot)
    {
      ++week;
    }

    CityHistorySnapshot next;

    if (!readCityHistorySnapshot(types, graph, week, base, next, &error))
    {
      return false;
    }

    const auto before = history.events().size();

    if (!history.observe(next, &error))
    {
      return false;
    }

    if (snapshot)
    {
      const std::vector<CityHistoryEvent> delta(history.events().begin() + long(before), history.events().end());

      if (!cityPopulationReconciles(*snapshot, next, delta, &error))
      {
        return false;
      }
    }

    if (!homes.observe(next, &error) || !homes.validate(next, &error))
    {
      return false;
    }

    snapshot = next;

    return true;
  }

  const CityHousehold &home(uint32_t person) const
  {
    return homes.households().at(*homes.householdOf(person));
  }

  uint32_t liveHouseholds(uint32_t settlement) const
  {
    return homes.livingHouseholds(settlement);
  }

  bool has(CityHouseholdEventKind kind, uint32_t atWeek) const
  {
    for (const auto &e : homes.events())
    {
      if (e.kind == kind && e.week == atWeek)
      {
        return true;
      }
    }

    return false;
  }
};

} // namespace cityfixture
