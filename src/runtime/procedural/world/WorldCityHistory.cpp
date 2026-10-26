#include "WorldCityHistory.hpp"
#include <algorithm>
#include <functional>
#include <set>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *error, const std::string &message)
{
  if (error)
  {
    *error = message;
  }
  return false;
}
} // namespace

bool readCityHistorySnapshot(const graphrewrite::compiler::EdgeTypeTable &types, const graphrewrite::runtime::EdgePageStore &graph, uint32_t week, uint32_t dateBase, CityHistorySnapshot &out, std::string *error)
{
  using namespace graphrewrite;
  using runtime::KeyOrder;
  CityHistorySnapshot next;
  next.week = week;
  bool ok = true;
  auto edges = [&](const char *pattern, bool required = false) {
    compiler::EdgeTypeId type;
    if (!types.find(pattern, type))
    {
      if (required)
      {
        ok = false;
        fail(error, std::string("missing history schema: ") + pattern);
      }
      return std::vector<runtime::Edge>{};
    }
    if (graph.has(type, KeyOrder::ByOrigin))
    {
      return graph.partition(type, KeyOrder::ByOrigin).snapshot();
    }
    if (graph.has(type, KeyOrder::ByTarget))
    {
      return graph.partition(type, KeyOrder::ByTarget).snapshot();
    }
    // A declared but unindexed field would silently erase history.
    ok = false;
    fail(error, std::string("unindexed history schema: ") + pattern);
    return std::vector<runtime::Edge>{};
  };
  const auto born = edges("? born in ?", true);
  for (auto e : born)
  {
    if (e.target < dateBase || e.target - dateBase > week || next.people.count(e.origin))
    {
      return fail(error, "invalid or duplicate birth date");
    }
    auto &p = next.people[e.origin];
    p.id = e.origin;
    p.bornWeek = e.target - dateBase;
    const auto age = (week - p.bornWeek) / 52;
    p.stage = age < 12 ? CityLifeStage::Child : age < 18 ? CityLifeStage::ApprenticeAge : age < 65 ? CityLifeStage::Adult : CityLifeStage::Elder;
  }
  std::set<HistoryNode> living, dead;
  for (auto e : edges("? is alive", true))
  {
    living.insert(e.origin);
  }
  for (auto e : edges("? is dead", true))
  {
    dead.insert(e.origin);
  }
  for (auto id : living)
  {
    if (!next.people.count(id) || dead.count(id))
    {
      return fail(error, "living person has no birth date or is also dead");
    }
  }
  for (auto id : dead)
  {
    if (!next.people.count(id))
    {
      return fail(error, "dead ancestor has no birth date");
    }
  }
  for (auto &[id, p] : next.people)
  {
    if (!living.count(id) && !dead.count(id))
    {
      return fail(error, "person has no life state");
    }
    p.alive = living.count(id) != 0;
  }
  auto assign = [&](const char *pattern, std::optional<HistoryNode> CityHistoryPerson::*field, bool required) {
    for (auto e : edges(pattern, required))
    {
      auto it = next.people.find(e.origin);
      if (it == next.people.end())
      {
        ok = false;
        fail(error, "relationship refers to unknown person");
        continue;
      }
      auto &v = it->second.*field;
      if (v && *v != e.target)
      {
        ok = false;
        fail(error, "person has conflicting relationships");
      }
      v = e.target;
    }
  };
  assign("? lives in ?", &CityHistoryPerson::settlement, true);
  assign("? is of house ?", &CityHistoryPerson::dynasty, true);
  // Marriage cleanup in the story can lag death by one tick. Retain its facts;
  // demand always filters living residents independently.
  assign("? is husband of ?", &CityHistoryPerson::spouse, false);
  assign("? is wife of ?", &CityHistoryPerson::spouse, false);
  for (auto e : edges("? is child of ?", true))
  {
    if (!next.people.count(e.origin) || !next.people.count(e.target) || next.people.at(e.origin).bornWeek <= next.people.at(e.target).bornWeek)
    {
      return fail(error, "invalid ancestry: parents must exist and predate children");
    }
    next.people.at(e.origin).parents.push_back(e.target);
  }
  for (auto &[id, p] : next.people)
  {
    std::sort(p.parents.begin(), p.parents.end());
    p.parents.erase(std::unique(p.parents.begin(), p.parents.end()), p.parents.end());
    if (p.spouse && (!next.people.count(*p.spouse) || *p.spouse == id))
    {
      return fail(error, "invalid spouse reference");
    }
  }
  auto roles = [&](const char *pattern, CitySocialRole role) {
    for (auto e : edges(pattern))
    {
      auto it = next.people.find(e.origin);
      if (it == next.people.end())
      {
        ok = false;
        fail(error, "office held by unknown person");
        continue;
      }
      if (!it->second.alive)
      {
        continue; // Story releases dead officeholders next tick.
      }
      if (role > it->second.role)
      {
        it->second.role = role;
      }
      if (role == CitySocialRole::Ruler && !next.rulers.emplace(e.target, e.origin).second)
      {
        ok = false;
        fail(error, "realm has multiple living rulers");
      }
    }
  };
  roles("? serves ?", CitySocialRole::Clergy);
  roles("? is heir to ?", CitySocialRole::Heir);
  roles("? is queen of ?", CitySocialRole::Consort);
  roles("? is king of ?", CitySocialRole::Ruler);
  // Story cleanup of these offices can lag a death or a crowning by a tick;
  // the lowest living id holds until the rules settle, deterministically.
  auto livingOffice = [&](const char *pattern, std::map<HistoryNode, HistoryNode> &out) {
    for (auto e : edges(pattern))
    {
      const auto it = next.people.find(e.origin);
      if (it == next.people.end() || !it->second.alive)
      {
        continue;
      }
      const auto held = out.find(e.target);
      if (held == out.end() || e.origin < held->second)
      {
        out[e.target] = e.origin;
      }
    }
  };
  livingOffice("? is heir to ?", next.heirs);
  livingOffice("? is queen of ?", next.consorts);
  livingOffice("? is head of ?", next.houseHeads);
  for (auto e : edges("? claims ?"))
  {
    const auto it = next.people.find(e.origin);
    if (it != next.people.end() && it->second.alive)
    {
      next.claimants[e.target].insert(e.origin);
    }
  }
  for (auto e : edges("? wants a king"))
  {
    next.vacantRealms.insert(e.origin);
  }
  for (auto e : edges("? swears to ?"))
  {
    next.sworn[e.origin] = e.target;
  }
  for (auto e : edges("? feuds with ?"))
  {
    next.feuds.insert({e.origin, e.target});
  }
  if (!ok)
  {
    return false;
  }
  out = std::move(next);
  if (error)
  {
    error->clear();
  }
  return true;
}

bool CityHistoryLedger::observe(const CityHistorySnapshot &next, std::string *error)
{
  if (!latest)
  {
    latest = next;
    if (error)
    {
      error->clear();
    }
    return true;
  }
  if (uint64_t(next.week) != uint64_t(latest->week) + 1)
  {
    return fail(error, "history observations must be consecutive weeks");
  }
  // Immutable identity and ancestry survive migration and death.
  for (const auto &[id, p] : latest->people)
  {
    auto it = next.people.find(id);
    if (it == next.people.end() || it->second.bornWeek != p.bornWeek || it->second.parents != p.parents || (!p.alive && it->second.alive))
    {
      return fail(error, "history lost an ancestor, changed lineage, or resurrected a person");
    }
  }
  std::vector<CityHistoryEvent> delta;
  auto add = [&](CityHistoryEventKind kind, HistoryNode id, std::optional<HistoryNode> before = {}, std::optional<HistoryNode> after = {}) {
    delta.push_back({next.week, kind, id, before, after});
  };
  for (const auto &[id, p] : next.people)
  {
    const auto it = latest->people.find(id);
    if (it == latest->people.end())
    {
      if (p.bornWeek == next.week)
      {
        add(CityHistoryEventKind::Birth, id, {}, p.settlement);
      }
      else if (p.alive)
      {
        add(CityHistoryEventKind::Arrival, id, {}, p.settlement);
      }
      continue;
    }
    const auto &old = it->second;
    if (old.alive && !p.alive)
    {
      add(CityHistoryEventKind::Death, id, old.settlement, {});
    }
    if (old.alive && p.alive && old.settlement != p.settlement)
    {
      add(!old.settlement ? CityHistoryEventKind::Arrival : !p.settlement ? CityHistoryEventKind::Departure : CityHistoryEventKind::Move, id, old.settlement, p.settlement);
    }
    if (p.spouse != old.spouse)
    {
      // One event per person: no assumption that one directional edge is a couple.
      if (old.spouse)
      {
        add(CityHistoryEventKind::Separation, id, old.spouse, {});
      }
      if (p.spouse)
      {
        add(CityHistoryEventKind::Marriage, id, {}, p.spouse);
      }
    }
    if (p.alive && p.role != old.role)
    {
      add(CityHistoryEventKind::RoleChange, id, uint32_t(old.role), uint32_t(p.role));
    }
    if (p.alive && p.stage != old.stage)
    {
      add(CityHistoryEventKind::LifeStageChange, id, uint32_t(old.stage), uint32_t(p.stage));
    }
  }
  std::set<HistoryNode> realms;
  for (const auto &[id, ruler] : latest->rulers)
  {
    realms.insert(id);
  }
  for (const auto &[id, ruler] : next.rulers)
  {
    realms.insert(id);
  }
  for (auto id : realms)
  {
    std::optional<HistoryNode> before, after;
    if (latest->rulers.count(id))
    {
      before = latest->rulers.at(id);
    }
    if (next.rulers.count(id))
    {
      after = next.rulers.at(id);
    }
    if (before != after)
    {
      add(CityHistoryEventKind::Succession, id, before, after);
    }
  }
  for(auto [a,b]:next.feuds)if(a<b && !latest->feuds.count({a,b}))
    add(CityHistoryEventKind::FeudStarted,a,{},b);
  for(auto [a,b]:latest->feuds)if(a<b && !next.feuds.count({a,b}))
    add(CityHistoryEventKind::FeudEnded,a,b,{});
  latest = next;
  log.insert(log.end(), delta.begin(), delta.end());
  if (error)
  {
    error->clear();
  }
  return true;
}

bool cityPopulationReconciles(const CityHistorySnapshot &before, const CityHistorySnapshot &after, const std::vector<CityHistoryEvent> &weekEvents, std::string *error)
{
  if (uint64_t(after.week) != uint64_t(before.week) + 1)
  {
    return fail(error, "population reconciliation needs consecutive weeks");
  }

  std::map<HistoryNode, int64_t> expected;

  for (const auto &[id, p] : before.people)
  {
    if (p.alive && p.settlement)
    {
      ++expected[*p.settlement];
    }
  }

  for (const auto &e : weekEvents)
  {
    if (e.week != after.week)
    {
      return fail(error, "population reconciliation given events of another week");
    }

    switch (e.kind)
    {
    case CityHistoryEventKind::Birth:
    case CityHistoryEventKind::Arrival:
      if (e.after)
      {
        ++expected[*e.after];
      }

      break;
    case CityHistoryEventKind::Death:
    case CityHistoryEventKind::Departure:
      if (e.before)
      {
        --expected[*e.before];
      }

      break;
    case CityHistoryEventKind::Move:
      if (e.before)
      {
        --expected[*e.before];
      }

      if (e.after)
      {
        ++expected[*e.after];
      }

      break;
    default:
      break;
    }
  }

  std::map<HistoryNode, int64_t> actual;

  for (const auto &[id, p] : after.people)
  {
    if (p.alive && p.settlement)
    {
      ++actual[*p.settlement];
    }
  }

  for (auto it = expected.begin(); it != expected.end();)
  {
    it = it->second == 0 ? expected.erase(it) : std::next(it);
  }

  if (actual != expected)
  {
    return fail(error, "births, deaths, arrivals and departures do not reconcile the population");
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

CityHistoryDemand cityHistoryDemand(const CityHistorySnapshot &snapshot, HistoryNode settlement)
{
  CityHistoryDemand result;
  std::map<uint64_t, SettlementHousehold> cohorts;
  for (const auto &[id, p] : snapshot.people)
  {
    if (p.alive && p.settlement == settlement)
    {
      ++result.residents;
      // Tagged IDs distinguish a dynasty cohort from a person without a dynasty.
      const uint64_t key = p.dynasty ? uint64_t(*p.dynasty) << 1 : (uint64_t(id) << 1) | 1;
      auto &h = cohorts[key];
      h.id = key + 1;
      ++h.members;
      switch (p.stage)
      {
      case CityLifeStage::Child:
        ++result.children;
        break;
      case CityLifeStage::ApprenticeAge:
        ++result.apprenticeAge;
        break;
      case CityLifeStage::Adult:
        ++result.workingAge;
        ++h.workers;
        break;
      case CityLifeStage::Elder:
        ++result.elders;
        break;
      }
    }
  }
  for (const auto &[id, h] : cohorts)
  {
    result.familyCohorts.push_back(h);
  }
  return result;
}

std::vector<SettlementHousehold> cityHistoryHousingEstimates(const CityHistoryDemand &demand)
{
  std::vector<SettlementHousehold> result;
  for (auto cohort : demand.familyCohorts)
  {
    uint32_t part = 0;
    while (cohort.members)
    {
      const uint32_t members = std::min(8u, cohort.members);
      const uint32_t workers = std::min(members, cohort.workers);
      result.push_back({(cohort.id << 30) | ++part, members, workers, false});
      cohort.members -= members;
      cohort.workers -= workers;
    }
  }
  return result;
}

const char *cityHistoryEventName(CityHistoryEventKind kind)
{
  switch (kind)
  {
  case CityHistoryEventKind::Arrival:
    return "arrival";
  case CityHistoryEventKind::Departure:
    return "departure";
  case CityHistoryEventKind::Birth:
    return "birth";
  case CityHistoryEventKind::Death:
    return "death";
  case CityHistoryEventKind::Move:
    return "move";
  case CityHistoryEventKind::Marriage:
    return "marriage";
  case CityHistoryEventKind::Separation:
    return "separation";
  case CityHistoryEventKind::Succession:
    return "succession";
  case CityHistoryEventKind::RoleChange:
    return "role-change";
  case CityHistoryEventKind::LifeStageChange:
    return "life-stage-change";
  case CityHistoryEventKind::FeudStarted: return "feud-started";
  case CityHistoryEventKind::FeudEnded: return "feud-ended";
  }
  return "unknown";
}
} // namespace procedural::world::rewrite
