#include "WorldCityMigration.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::world::rewrite {
namespace {
using graphrewrite::compiler::EdgeTypeId;
using graphrewrite::runtime::Edge;
using graphrewrite::runtime::EdgePageStore;
using graphrewrite::runtime::Gender;
using graphrewrite::runtime::KeyOrder;

bool fail(std::string *error, const std::string &message)
{
  if (error)
  {
    *error = message;
  }

  return false;
}

uint64_t mix(uint64_t x)
{
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;

  return x ^ (x >> 31);
}

// A seeded stream: the same seed, week and counter always compose the same
// family, whatever else the story did.
struct Dice
{
  uint64_t state;

  uint32_t roll(uint32_t sides)
  {
    state = mix(state);

    return sides ? uint32_t(state >> 33) % sides : 0;
  }
};

std::vector<Edge> edgesOf(const EdgePageStore &graph, EdgeTypeId type)
{
  if (graph.has(type, KeyOrder::ByOrigin))
  {
    return graph.partition(type, KeyOrder::ByOrigin).snapshot();
  }

  if (graph.has(type, KeyOrder::ByTarget))
  {
    return graph.partition(type, KeyOrder::ByTarget).snapshot();
  }

  return {};
}

double clamp01(double v)
{
  return std::min(1.0, std::max(0.0, v));
}
} // namespace

double cityMigrationAttraction(const CitySettlementProspects &p)
{
  return clamp01(0.4 * clamp01(p.work) + 0.4 * clamp01(p.food) + 0.2 * clamp01(p.safety));
}

CityMigrationAdapter::CityMigrationAdapter(CityMigrationPolicy migration, const graphrewrite::compiler::EdgeTypeTable &table, EdgePageStore &store, graphrewrite::runtime::NameTable &nameTable, uint32_t dateBase)
    : policy(std::move(migration)), types(table), graph(store), names(nameTable), dayBase(dateBase)
{
  auto find = [&](const char *pattern, EdgeTypeId &id) {
    if (!types.find(pattern, id) && schemaError.empty())
    {
      schemaError = std::string("missing migration schema: ") + pattern;
    }
  };

  find("? is alive", schema.alive);
  find("? is man", schema.man);
  find("? is woman", schema.woman);
  find("? born in ?", schema.born);
  find("? is young", schema.young);
  find("? is an infant", schema.infant);
  find("? is grown", schema.grown);
  find("? is unmarried", schema.unmarried);
  find("? is unattached", schema.unattached);
  find("? is fertile", schema.fertile);
  find("? is husband of ?", schema.husband);
  find("? is wife of ?", schema.wife);
  find("? is child of ?", schema.child);
  find("? is mother of ?", schema.mother);
  find("? is father of ?", schema.father);
  find("? is of house ?", schema.house);
  find("? is head of ?", schema.head);
  find("? is at peace with ?", schema.peace);
  find("? lives in ?", schema.lives);
  find("? keeps the peace", schema.keepsPeace);

  if (policy.origins.empty() && !policy.destinations.empty() && schemaError.empty())
  {
    schemaError = "migration policy opens a destination but names no origin settlement";
  }

  if (!schemaError.empty())
  {
    return;
  }

  for (const auto &name : policy.origins)
  {
    const auto node = graph.allocateVertex();

    names.setName(node, name);
    originNodes.push_back(node);
    originNames[node] = name;
  }
}

bool CityMigrationAdapter::ready(std::string *error) const
{
  if (!schemaError.empty())
  {
    return fail(error, schemaError);
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

bool CityMigrationAdapter::isOrigin(HistoryNode node) const
{
  return originNames.count(node) != 0;
}

bool CityMigrationAdapter::roomFor(HistoryNode settlement, const CityHouseholdLedger &ledger, uint32_t promised) const
{
  const auto capacity = ledger.policy().housingCapacity.find(settlement);

  if (capacity == ledger.policy().housingCapacity.end())
  {
    return true;
  }

  const uint32_t waiting = uint32_t(ledger.waitingList(settlement).size()) + promised;

  return ledger.housedHouseholds(settlement) + waiting < capacity->second || waiting < policy.waitingTolerance;
}

bool CityMigrationAdapter::hasKinAt(const CityHousehold &h, HistoryNode settlement, const CityHistorySnapshot &snapshot) const
{
  for (auto member : h.members)
  {
    const auto it = snapshot.people.find(member);

    if (it == snapshot.people.end())
    {
      continue;
    }

    const auto &m = it->second;

    for (const auto &[id, q] : snapshot.people)
    {
      if (!q.alive || q.settlement != settlement || id == member)
      {
        continue;
      }

      const bool house = q.dynasty && q.dynasty == m.dynasty;
      const bool blood = std::binary_search(m.parents.begin(), m.parents.end(), id) || std::binary_search(q.parents.begin(), q.parents.end(), member);

      if (house || blood || m.spouse == id)
      {
        return true;
      }
    }
  }

  return false;
}

std::vector<HistoryNode> CityMigrationAdapter::move(const std::vector<HistoryNode> &people, HistoryNode from, HistoryNode to, const CityHistorySnapshot &snapshot)
{
  std::vector<HistoryNode> moved;

  for (auto id : people)
  {
    const auto it = snapshot.people.find(id);

    // The dead stay where they lie; someone the story moved is not ours.
    if (it == snapshot.people.end() || !it->second.alive || it->second.settlement != from)
    {
      continue;
    }

    graph.removeEdge(schema.lives, Edge{id, from});
    graph.addEdge(schema.lives, Edge{id, to});
    moved.push_back(id);
  }

  return moved;
}

void CityMigrationAdapter::makePeace(const std::vector<HistoryNode> &people, HistoryNode settlement, const CityHistorySnapshot &snapshot)
{
  std::set<HistoryNode> theirs, local;

  for (const auto e : edgesOf(graph, schema.house))
  {
    if (std::find(people.begin(), people.end(), e.origin) != people.end())
    {
      theirs.insert(e.target);
    }
  }

  for (const auto e : edgesOf(graph, schema.head))
  {
    const auto it = snapshot.people.find(e.origin);

    if (it != snapshot.people.end() && it->second.alive && it->second.settlement == settlement)
    {
      local.insert(e.target);
    }
  }

  for (auto a : theirs)
  {
    for (auto b : local)
    {
      if (a == b)
      {
        continue;
      }

      for (const Edge edge : {Edge{a, b}, Edge{b, a}})
      {
        if (!graph.containsEdge(schema.peace, edge))
        {
          graph.addEdge(schema.peace, edge);
        }
      }
    }
  }
}

HistoryNode CityMigrationAdapter::spawnPerson(uint32_t week, bool man, uint32_t ageYears, uint32_t weekOffset, HistoryNode origin, HistoryNode house, bool adult, bool single)
{
  const auto id = graph.allocateVertex();
  auto mark = [&](EdgeTypeId type) { graph.addEdge(type, Edge{id, id}); };

  mark(schema.alive);
  mark(man ? schema.man : schema.woman);
  graph.addEdge(schema.born, Edge{id, dayBase + (week - ageYears * 52 - weekOffset)});
  graph.addEdge(schema.lives, Edge{id, origin});
  graph.addEdge(schema.house, Edge{id, house});

  if (adult)
  {
    mark(schema.grown);
    mark(schema.keepsPeace);

    if (single)
    {
      mark(schema.unmarried);
      mark(schema.unattached);
    }

    if (!man && ageYears < 45)
    {
      mark(schema.fertile);
    }
  }
  else
  {
    mark(schema.young);

    if (ageYears < 5)
    {
      mark(schema.infant);
    }
  }

  return id;
}

CityMigrationAdapter::Pending CityMigrationAdapter::generate(uint32_t week, HistoryNode destination)
{
  Dice dice{mix(uint64_t(policy.seed) * 0x9e3779b97f4a7c15ull + week) ^ (uint64_t(++generated) << 32)};
  Pending p;

  p.origin = originNodes[dice.roll(uint32_t(originNodes.size()))];
  p.destination = destination;

  const auto house = graph.allocateVertex();
  std::string surname = graphrewrite::runtime::generateName(policy.seed, house, Gender::Masculine);

  while (!surname.empty() && std::string("aeiouy").find(char(std::tolower(surname.back()))) != std::string::npos)
  {
    surname.pop_back();
  }

  names.setName(house, surname + "ing");

  const uint32_t type = dice.roll(100);
  const bool couple = type < 75;
  const uint32_t children = type < 60 ? 1 + dice.roll(3) : 0;
  HistoryNode head;

  if (couple)
  {
    const uint32_t manAge = 20 + dice.roll(20), womanAge = 18 + dice.roll(20);
    const auto man = spawnPerson(week, true, manAge, 1 + dice.roll(51), p.origin, house, true, false);
    const auto woman = spawnPerson(week, false, womanAge, 1 + dice.roll(51), p.origin, house, true, false);

    graph.addEdge(schema.husband, Edge{man, woman});
    graph.addEdge(schema.wife, Edge{woman, man});
    p.people = {man, woman};
    head = man;

    const uint32_t oldest = std::min(11u, std::min(manAge, womanAge) - 17);

    for (uint32_t i = 0; i < children; ++i)
    {
      const auto child = spawnPerson(week, dice.roll(2) == 0, dice.roll(oldest + 1), 1 + dice.roll(51), p.origin, house, false, false);

      graph.addEdge(schema.child, Edge{child, man});
      graph.addEdge(schema.child, Edge{child, woman});
      graph.addEdge(schema.father, Edge{man, child});
      graph.addEdge(schema.mother, Edge{woman, child});
      p.people.push_back(child);
    }
  }
  else
  {
    head = spawnPerson(week, dice.roll(2) == 0, 18 + dice.roll(30), 1 + dice.roll(51), p.origin, house, true, true);
    p.people = {head};
  }

  graph.addEdge(schema.head, Edge{head, house});

  return p;
}

bool CityMigrationAdapter::publish(uint32_t week, const CityHistorySnapshot &snapshot, const CityHouseholdLedger &ledger, std::string *error)
{
  if (!ready(error))
  {
    return false;
  }

  if (snapshot.week != week || ledger.week() != std::optional<uint32_t>(week))
  {
    return fail(error, "migration publishes from the validated ledgers of the week just closed");
  }

  for (const auto &p : pending)
  {
    const auto moved = move(p.people, p.origin, p.destination, snapshot);

    if (!moved.empty())
    {
      makePeace(moved, p.destination, snapshot);
      log.push_back({week, CityMigrationEventKind::Arrived, p.origin, p.destination, 0, moved});
    }
  }

  pending.clear();

  if (policy.reviewIntervalWeeks == 0 || (week + 1) % policy.reviewIntervalWeeks != 0)
  {
    return true;
  }

  uint32_t living = 0;

  for (const auto &[id, p] : snapshot.people)
  {
    living += p.alive;
  }

  for (const auto &[place, prospects] : policy.destinations)
  {
    const HistoryNode destination = place;
    const double attraction = cityMigrationAttraction(prospects);
    std::vector<const CityHousehold *> local;

    for (const auto &[id, h] : ledger.households())
    {
      if (!h.dissolvedWeek && h.settlement == destination)
      {
        local.push_back(&h);
      }
    }

    std::set<HouseholdId> departed;
    auto depart = [&](const CityHousehold &h) {
      const bool home = h.origin && isOrigin(*h.origin);
      const auto to = home ? *h.origin : originNodes[Dice{mix(uint64_t(policy.seed) ^ (uint64_t(week) << 32) ^ h.id)}.roll(uint32_t(originNodes.size()))];
      const auto moved = move(std::vector<HistoryNode>(h.members.begin(), h.members.end()), destination, to, snapshot);

      departed.insert(h.id);
      log.push_back({week, CityMigrationEventKind::Departed, destination, to, h.id, moved});
    };

    for (const auto *h : local)
    {
      if (h->housing == CityHousingState::Waiting && uint64_t(*h->waitingSince) + policy.patienceWeeks <= week)
      {
        depart(*h);
      }
    }

    if (attraction < policy.emigrationThreshold)
    {
      const CityHousehold *newest = nullptr;

      for (const auto *h : local)
      {
        if (!departed.count(h->id) && (!newest || std::make_pair(h->foundedWeek, h->id) > std::make_pair(newest->foundedWeek, newest->id)))
        {
          newest = h;
        }
      }

      if (newest)
      {
        depart(*newest);
      }
    }

    uint32_t budget = attraction >= policy.arrivalThreshold ? uint32_t(std::lround(attraction * policy.householdsPerReview)) : 0;
    uint32_t promised = 0;

    // Those who left keep their claim: kin and a free home bring them back first.
    for (const auto &[id, h] : ledger.households())
    {
      if (!budget || h.dissolvedWeek || !h.settlement || !isOrigin(*h.settlement) || h.origin != destination || !roomFor(destination, ledger, promised) || !hasKinAt(h, destination, snapshot))
      {
        continue;
      }

      const auto moved = move(std::vector<HistoryNode>(h.members.begin(), h.members.end()), *h.settlement, destination, snapshot);

      if (moved.empty())
      {
        continue;
      }

      makePeace(moved, destination, snapshot);
      log.push_back({week, CityMigrationEventKind::Returned, *h.settlement, destination, id, moved});
      --budget;
      ++promised;
    }

    if (budget && living >= policy.maxLivingPeople)
    {
      log.push_back({week, CityMigrationEventKind::Suspended, {}, destination, 0, {}});
      budget = 0;
    }

    while (budget && living < policy.maxLivingPeople && roomFor(destination, ledger, promised))
    {
      auto p = generate(week, destination);

      log.push_back({week, CityMigrationEventKind::Generated, p.origin, p.destination, 0, p.people});
      living += uint32_t(p.people.size());
      pending.push_back(std::move(p));
      --budget;
      ++promised;
    }
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

const char *cityMigrationEventName(CityMigrationEventKind kind)
{
  switch (kind)
  {
  case CityMigrationEventKind::Generated:
    return "generated";
  case CityMigrationEventKind::Arrived:
    return "arrived";
  case CityMigrationEventKind::Departed:
    return "departed";
  case CityMigrationEventKind::Returned:
    return "returned";
  case CityMigrationEventKind::Suspended:
    return "suspended";
  }

  return "unknown";
}
} // namespace procedural::world::rewrite
