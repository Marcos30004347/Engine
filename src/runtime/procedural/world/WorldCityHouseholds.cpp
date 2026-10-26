#include "WorldCityHouseholds.hpp"
#include <algorithm>
#include <iterator>
#include <tuple>

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

bool guardianAge(CityLifeStage stage)
{
  return stage == CityLifeStage::Adult || stage == CityLifeStage::Elder;
}

bool dependentAge(CityLifeStage stage)
{
  return stage == CityLifeStage::Child || stage == CityLifeStage::ApprenticeAge;
}

uint32_t headRank(CityLifeStage stage)
{
  switch (stage)
  {
  case CityLifeStage::Adult:
    return 0;
  case CityLifeStage::Elder:
    return 1;
  case CityLifeStage::ApprenticeAge:
    return 2;
  case CityLifeStage::Child:
    return 3;
  }

  return 4;
}
} // namespace

// One week's transaction over a working copy of the ledger state.
class CityHouseholdLedger::Week
{
public:
  Week(const CityHouseholdPolicy &policy, State &state, const CityHistorySnapshot &snapshot) : policy(policy), state(state), snapshot(snapshot)
  {
  }

  void seed()
  {
    std::map<std::pair<HistoryNode, uint64_t>, std::vector<HistoryNode>> cohorts;

    for (const auto &[id, p] : snapshot.people)
    {
      if (p.alive && p.settlement)
      {
        // Tagged key: a house, or a person of no house who lives alone.
        const uint64_t key = p.dynasty ? uint64_t(*p.dynasty) << 1 : (uint64_t(id) << 1) | 1;

        cohorts[{*p.settlement, key}].push_back(id);
      }
    }

    for (const auto &[key, people] : cohorts)
    {
      const auto id = found(key.first, CityHouseholdOrigin::Founding, {}, "founding");

      for (auto person : people)
      {
        join(id, person, "founding");
      }
    }

    finish();
  }

  void update()
  {
    departures();
    moves();
    arrivals();
    recomputeHeads();
    marriages();
    independence();
    finish();
  }

private:
  const CityHouseholdPolicy &policy;
  State &state;
  const CityHistorySnapshot &snapshot;

  const CityHistoryPerson *person(HistoryNode id) const
  {
    const auto it = snapshot.people.find(id);

    return it == snapshot.people.end() ? nullptr : &it->second;
  }

  CityHousehold &household(HouseholdId id)
  {
    return state.households.at(id);
  }

  std::optional<HouseholdId> of(HistoryNode person) const
  {
    const auto it = state.index.find(person);

    return it == state.index.end() ? std::nullopt : std::optional<HouseholdId>(it->second);
  }

  void add(CityHouseholdEventKind kind, HouseholdId id, std::optional<HistoryNode> person, std::optional<HistoryNode> before, std::optional<HistoryNode> after, const char *cause)
  {
    state.log->push_back({state.week, kind, id, person, before, after, cause});
  }

  bool managed(std::optional<HistoryNode> settlement) const
  {
    return settlement && policy.housingCapacity.count(*settlement);
  }

  void enterSettlement(CityHousehold &h)
  {
    if (managed(h.settlement))
    {
      h.housing = CityHousingState::Waiting;
      h.waitingSince = state.week;
    }
    else
    {
      h.housing = CityHousingState::Unmanaged;
      h.waitingSince.reset();
    }
  }

  void release(CityHousehold &h, const char *cause)
  {
    if (h.housing == CityHousingState::Housed)
    {
      add(CityHouseholdEventKind::HomeReleased, h.id, {}, h.settlement, {}, cause);
    }

    h.housing = CityHousingState::Unmanaged;
    h.waitingSince.reset();
  }

  HouseholdId found(HistoryNode settlement, CityHouseholdOrigin kind, std::optional<HistoryNode> origin, const char *cause)
  {
    CityHousehold h;

    h.id = householdIdTag | state.nextSequence++;
    h.foundedWeek = state.week;
    h.settlement = settlement;
    h.origin = origin;
    h.kind = kind;
    enterSettlement(h);
    state.households.emplace(h.id, h);
    add(CityHouseholdEventKind::Founded, h.id, {}, origin, settlement, cause);

    return h.id;
  }

  void join(HouseholdId id, HistoryNode person, const char *cause)
  {
    household(id).members.insert(person);
    state.index[person] = id;
    add(CityHouseholdEventKind::Joined, id, person, {}, {}, cause);
  }

  void leave(HistoryNode person, const char *cause)
  {
    const auto id = state.index.at(person);
    auto &h = household(id);

    h.members.erase(person);
    state.index.erase(person);
    add(CityHouseholdEventKind::Left, id, person, h.settlement, {}, cause);
  }

  void relocate(HouseholdId id, HistoryNode destination, const char *cause)
  {
    auto &h = household(id);
    const auto from = h.settlement;

    release(h, cause);
    add(CityHouseholdEventKind::Relocated, id, {}, from, destination, cause);
    h.origin = from;
    h.kind = CityHouseholdOrigin::Migration;
    h.settlement = destination;
    enterSettlement(h);
  }

  // Dependent children of `parent` living in household `within`.
  std::vector<HistoryNode> dependentsOf(HistoryNode parent, HouseholdId within) const
  {
    std::vector<HistoryNode> result;

    for (auto member : state.households.at(within).members)
    {
      const auto *p = person(member);

      if (member != parent && p && dependentAge(p->stage) && std::binary_search(p->parents.begin(), p->parents.end(), parent))
      {
        result.push_back(member);
      }
    }

    return result;
  }

  // A live household at `settlement` other than `excluding`, in kin order:
  // parent, grandparent, adult sibling, then the head of the child's house.
  std::optional<HouseholdId> guardianFor(HistoryNode child, HistoryNode settlement, std::optional<HouseholdId> excluding) const
  {
    const auto *c = person(child);

    if (!c)
    {
      return std::nullopt;
    }

    auto at = [&](HistoryNode q) -> std::optional<HouseholdId> {
      const auto *p = person(q);
      const auto id = of(q);

      if (p && p->alive && id && id != excluding && state.households.at(*id).settlement == settlement)
      {
        return id;
      }

      return std::nullopt;
    };

    for (auto parent : c->parents)
    {
      if (const auto id = at(parent))
      {
        return id;
      }
    }

    for (auto parent : c->parents)
    {
      if (const auto *p = person(parent))
      {
        for (auto grandparent : p->parents)
        {
          if (const auto id = at(grandparent))
          {
            return id;
          }
        }
      }
    }

    for (const auto &[id, q] : snapshot.people)
    {
      if (id == child || !q.alive || !guardianAge(q.stage))
      {
        continue;
      }

      std::vector<HistoryNode> shared;

      std::set_intersection(c->parents.begin(), c->parents.end(), q.parents.begin(), q.parents.end(), std::back_inserter(shared));

      if (!shared.empty())
      {
        if (const auto found = at(id))
        {
          return found;
        }
      }
    }

    if (c->dynasty)
    {
      for (const auto &[id, h] : state.households)
      {
        if (id != excluding && !h.dissolvedWeek && h.settlement == settlement && h.head)
        {
          const auto *head = person(*h.head);

          if (head && head->dynasty == c->dynasty)
          {
            return id;
          }
        }
      }
    }

    return std::nullopt;
  }

  void place(HistoryNode id, HistoryNode settlement, std::optional<HistoryNode> from, const char *cause)
  {
    const auto *p = person(id);

    if (p->spouse)
    {
      const auto *spouse = person(*p->spouse);
      const auto home = of(*p->spouse);

      if (spouse && spouse->alive && home && household(*home).settlement == settlement)
      {
        join(*home, id, cause);

        return;
      }
    }

    if (dependentAge(p->stage))
    {
      if (const auto guardian = guardianFor(id, settlement, {}))
      {
        join(*guardian, id, cause);

        return;
      }
    }

    const auto kind = from ? CityHouseholdOrigin::Migration : p->bornWeek == state.week ? CityHouseholdOrigin::LocalFormation : CityHouseholdOrigin::Unknown;

    join(found(settlement, kind, from, cause), id, cause);
  }

  void departures()
  {
    for (const auto [id, home] : std::map<HistoryNode, HouseholdId>(state.index))
    {
      const auto *p = person(id);

      if (!p || !p->alive)
      {
        leave(id, "death");
      }
      else if (!p->settlement)
      {
        leave(id, "departure");
      }
    }
  }

  void moves()
  {
    std::map<HouseholdId, std::vector<std::pair<HistoryNode, HistoryNode>>> movers;

    for (const auto &[id, home] : state.index)
    {
      const auto *p = person(id);

      if (p->settlement != household(home).settlement)
      {
        movers[home].push_back({id, *p->settlement});
      }
    }

    for (const auto &[home, group] : movers)
    {
      auto &h = household(home);
      const HistoryNode first = group.front().second;
      const bool whole = group.size() == h.members.size() && std::all_of(group.begin(), group.end(), [first](const auto &m) { return m.second == first; });

      if (whole)
      {
        relocate(home, group.front().second, "move");

        continue;
      }

      for (const auto &[id, destination] : group)
      {
        const auto from = h.settlement;

        leave(id, "move");
        place(id, destination, from, "move");
      }
    }
  }

  void arrivals()
  {
    std::vector<std::pair<uint32_t, HistoryNode>> newcomers;

    for (const auto &[id, p] : snapshot.people)
    {
      if (p.alive && p.settlement && !state.index.count(id))
      {
        newcomers.push_back({p.bornWeek, id});
      }
    }

    // Elders before their children, so a family arriving together is one home.
    std::sort(newcomers.begin(), newcomers.end());

    for (const auto &[born, id] : newcomers)
    {
      place(id, *person(id)->settlement, {}, born == state.week ? "birth" : "arrival");
    }
  }

  void transfer(const std::vector<HistoryNode> &people, HouseholdId to, const char *cause)
  {
    for (auto id : people)
    {
      leave(id, cause);
      join(to, id, cause);
    }
  }

  void marriages()
  {
    for (const auto [a, homeA] : std::map<HistoryNode, HouseholdId>(state.index))
    {
      const auto *pa = person(a);

      if (!pa->spouse || *pa->spouse <= a)
      {
        continue;
      }

      const auto b = *pa->spouse;
      const auto *pb = person(b);
      const auto homeB = of(b);

      if (!pb || !pb->alive || !homeB || *homeB == state.index.at(a))
      {
        continue;
      }

      const auto currentA = state.index.at(a);
      auto &A = household(currentA);
      auto &B = household(*homeB);

      if (A.settlement != B.settlement)
      {
        continue; // The graph has them apart; a later move reconciles it.
      }

      const bool headA = A.head == a, headB = B.head == b;

      if (headA && headB)
      {
        const bool keepA = A.foundedWeek < B.foundedWeek || (A.foundedWeek == B.foundedWeek && A.id < B.id);
        auto &survivor = keepA ? A : B;
        auto &absorbed = keepA ? B : A;

        transfer(std::vector<HistoryNode>(absorbed.members.begin(), absorbed.members.end()), survivor.id, "marriage");
      }
      else if (headA || headB)
      {
        const auto mover = headA ? b : a;
        const auto from = headA ? *homeB : currentA;
        auto people = dependentsOf(mover, from);

        people.insert(people.begin(), mover);
        transfer(people, headA ? currentA : *homeB, "marriage");
      }
      else
      {
        auto people = dependentsOf(a, currentA);
        auto theirs = dependentsOf(b, *homeB);

        people.insert(people.begin(), a);
        people.push_back(b);
        people.insert(people.end(), theirs.begin(), theirs.end());
        transfer(people, found(*A.settlement, CityHouseholdOrigin::LocalFormation, {}, "marriage"), "marriage");
      }

      recomputeHeads();
    }
  }

  bool freeHome(HistoryNode settlement) const
  {
    const auto capacity = policy.housingCapacity.find(settlement);

    if (capacity == policy.housingCapacity.end())
    {
      return true;
    }

    uint32_t claimed = 0;

    for (const auto &[id, h] : state.households)
    {
      claimed += !h.dissolvedWeek && h.settlement == settlement && h.housing != CityHousingState::Unmanaged;
    }

    return claimed < capacity->second;
  }

  void independence()
  {
    if (!policy.independenceAgeYears)
    {
      return;
    }

    for (const auto [id, home] : std::map<HistoryNode, HouseholdId>(state.index))
    {
      const auto *p = person(id);
      const auto &h = household(home);
      const bool married = p->spouse && person(*p->spouse) && person(*p->spouse)->alive;

      if (p->stage != CityLifeStage::Adult || h.head == id || married || (state.week - p->bornWeek) / 52 < *policy.independenceAgeYears || !freeHome(*h.settlement))
      {
        continue;
      }

      auto people = dependentsOf(id, home);

      people.insert(people.begin(), id);
      transfer(people, found(*h.settlement, CityHouseholdOrigin::LocalFormation, {}, "independence"), "independence");
      recomputeHeads();
    }
  }

  void guardianship()
  {
    std::vector<HouseholdId> ids;

    for (const auto &[id, h] : state.households)
    {
      ids.push_back(id);
    }

    for (const auto id : ids)
    {
      auto &h = household(id);

      if (h.dissolvedWeek || h.members.empty())
      {
        continue;
      }

      std::optional<HistoryNode> firstChild;
      bool guardian = false, elders = true;

      for (auto member : h.members)
      {
        const auto stage = person(member)->stage;

        guardian = guardian || guardianAge(stage);
        elders = elders && stage == CityLifeStage::Elder;

        if (dependentAge(stage) && !firstChild)
        {
          firstChild = member;
        }
      }

      h.dependentElders = elders;

      if (!firstChild || guardian)
      {
        h.unguarded = false;

        continue;
      }

      if (const auto kin = guardianFor(*firstChild, *h.settlement, id))
      {
        add(CityHouseholdEventKind::GuardianAssigned, *kin, firstChild, {}, household(*kin).head, "guardianship");
        transfer(std::vector<HistoryNode>(h.members.begin(), h.members.end()), *kin, "guardianship");
        h.unguarded = false;
      }
      else if (!h.unguarded)
      {
        h.unguarded = true;
        add(CityHouseholdEventKind::Unguarded, id, firstChild, {}, {}, "guardianship");
      }
    }
  }

  void dissolve()
  {
    for (auto &[id, h] : state.households)
    {
      if (!h.dissolvedWeek && h.members.empty())
      {
        release(h, "extinction");
        h.dissolvedWeek = state.week;
        h.head.reset();
        h.unguarded = h.dependentElders = false;
        add(CityHouseholdEventKind::Dissolved, id, {}, h.settlement, {}, "extinction");
      }
    }
  }

  void recomputeHeads()
  {
    for (auto &[id, h] : state.households)
    {
      std::optional<HistoryNode> best;
      std::tuple<uint32_t, uint32_t, HistoryNode> bestKey;

      for (auto member : h.members)
      {
        const auto *p = person(member);
        const std::tuple<uint32_t, uint32_t, HistoryNode> key{headRank(p->stage), p->bornWeek, member};

        if (!best || key < bestKey)
        {
          best = member;
          bestKey = key;
        }
      }

      h.head = best;
    }
  }

  void allocateHousing()
  {
    for (const auto &[settlement, capacity] : policy.housingCapacity)
    {
      uint32_t housed = 0;
      std::vector<std::pair<uint32_t, HouseholdId>> waiting;

      for (const auto &[id, h] : state.households)
      {
        if (h.dissolvedWeek || h.settlement != settlement)
        {
          continue;
        }

        if (h.housing == CityHousingState::Housed)
        {
          ++housed;
        }
        else if (h.housing == CityHousingState::Waiting)
        {
          waiting.push_back({*h.waitingSince, id});
        }
      }

      std::sort(waiting.begin(), waiting.end());

      for (const auto &[since, id] : waiting)
      {
        if (housed >= capacity)
        {
          break;
        }

        auto &h = household(id);

        h.housing = CityHousingState::Housed;
        h.waitingSince.reset();
        add(CityHouseholdEventKind::Housed, id, {}, {}, settlement, "housing");
        ++housed;
      }
    }
  }

  void finish()
  {
    recomputeHeads();
    guardianship();
    dissolve();
    recomputeHeads();
    allocateHousing();
  }
};

CityHouseholdLedger::CityHouseholdLedger(CityHouseholdPolicy policy) : rules(std::move(policy))
{
}

bool CityHouseholdLedger::observe(const CityHistorySnapshot &snapshot, std::string *error)
{
  const size_t mark = state.log->size();
  State next = state;

  if (next.seeded && uint64_t(snapshot.week) != uint64_t(next.week) + 1)
  {
    return fail(error, "household observations must be consecutive weeks");
  }

  next.week = snapshot.week;

  Week week(rules, next, snapshot);

  if (next.seeded)
  {
    week.update();
  }
  else
  {
    week.seed();
    next.seeded = true;
  }

  // Validate on a probe ledger so a failed week leaves this one untouched.
  CityHouseholdLedger probe(rules);

  probe.state = std::move(next);

  if (!probe.validate(snapshot, error))
  {
    state.log->resize(mark);

    return false;
  }

  state = std::move(probe.state);

  if (error)
  {
    error->clear();
  }

  return true;
}

std::optional<HouseholdId> CityHouseholdLedger::householdOf(HistoryNode person) const
{
  const auto it = state.index.find(person);

  return it == state.index.end() ? std::nullopt : std::optional<HouseholdId>(it->second);
}

std::vector<HouseholdId> CityHouseholdLedger::waitingList(HistoryNode settlement) const
{
  std::vector<std::pair<uint32_t, HouseholdId>> waiting;

  for (const auto &[id, h] : state.households)
  {
    if (!h.dissolvedWeek && h.settlement == settlement && h.housing == CityHousingState::Waiting)
    {
      waiting.push_back({*h.waitingSince, id});
    }
  }

  std::sort(waiting.begin(), waiting.end());

  std::vector<HouseholdId> result;

  for (const auto &[since, id] : waiting)
  {
    result.push_back(id);
  }

  return result;
}

uint32_t CityHouseholdLedger::housedHouseholds(HistoryNode settlement) const
{
  uint32_t count = 0;

  for (const auto &[id, h] : state.households)
  {
    count += !h.dissolvedWeek && h.settlement == settlement && h.housing == CityHousingState::Housed;
  }

  return count;
}

uint32_t CityHouseholdLedger::livingHouseholds(HistoryNode settlement) const
{
  uint32_t count = 0;

  for (const auto &[id, h] : state.households)
  {
    count += !h.dissolvedWeek && h.settlement == settlement;
  }

  return count;
}

bool CityHouseholdLedger::validate(const CityHistorySnapshot &snapshot, std::string *error) const
{
  if (!state.seeded || snapshot.week != state.week)
  {
    return fail(error, "household ledger validated against a different week");
  }

  std::map<HistoryNode, uint32_t> residents, housed;

  for (const auto &[id, p] : snapshot.people)
  {
    const auto it = state.index.find(id);

    if (!p.alive || !p.settlement)
    {
      if (it != state.index.end())
      {
        return fail(error, "a dead or absent person still occupies a household");
      }

      continue;
    }

    ++residents[*p.settlement];

    if (it == state.index.end())
    {
      return fail(error, "a living resident belongs to no household");
    }

    const auto &h = state.households.at(it->second);

    if (h.dissolvedWeek || h.settlement != p.settlement || !h.members.count(id))
    {
      return fail(error, "a resident's household is dissolved, elsewhere, or does not list them");
    }
  }

  std::map<HistoryNode, uint32_t> housedPeople;

  for (const auto &[id, h] : state.households)
  {
    if (h.dissolvedWeek)
    {
      if (!h.members.empty())
      {
        return fail(error, "a dissolved household still has members");
      }

      continue;
    }

    if (!h.settlement)
    {
      return fail(error, "a live household has no settlement");
    }

    for (auto member : h.members)
    {
      const auto it = state.index.find(member);

      if (it == state.index.end() || it->second != id)
      {
        return fail(error, "a person is listed by a household that is not theirs");
      }

      ++housedPeople[*h.settlement];
    }

    const bool managed = rules.housingCapacity.count(*h.settlement) != 0;

    if (managed == (h.housing == CityHousingState::Unmanaged) || (h.housing == CityHousingState::Waiting) != h.waitingSince.has_value())
    {
      return fail(error, "housing state disagrees with the settlement's housing policy");
    }

    if (managed)
    {
      housed[*h.settlement] += h.housing == CityHousingState::Housed;
    }
  }

  if (housedPeople != residents)
  {
    return fail(error, "household membership does not reconcile with the living residents");
  }

  for (const auto &[settlement, count] : housed)
  {
    if (count > rules.housingCapacity.at(settlement))
    {
      return fail(error, "more households housed than the settlement's capacity");
    }
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

const char *cityHouseholdEventName(CityHouseholdEventKind kind)
{
  switch (kind)
  {
  case CityHouseholdEventKind::Founded:
    return "founded";
  case CityHouseholdEventKind::Joined:
    return "joined";
  case CityHouseholdEventKind::Left:
    return "left";
  case CityHouseholdEventKind::Relocated:
    return "relocated";
  case CityHouseholdEventKind::Dissolved:
    return "dissolved";
  case CityHouseholdEventKind::Housed:
    return "housed";
  case CityHouseholdEventKind::HomeReleased:
    return "home-released";
  case CityHouseholdEventKind::GuardianAssigned:
    return "guardian-assigned";
  case CityHouseholdEventKind::Unguarded:
    return "unguarded";
  }

  return "unknown";
}

const char *cityHouseholdOriginName(CityHouseholdOrigin kind)
{
  switch (kind)
  {
  case CityHouseholdOrigin::Founding:
    return "founding";
  case CityHouseholdOrigin::LocalFormation:
    return "local";
  case CityHouseholdOrigin::Migration:
    return "migration";
  case CityHouseholdOrigin::Unknown:
    return "unknown";
  }

  return "unknown";
}

const char *cityHousingStateName(CityHousingState state)
{
  switch (state)
  {
  case CityHousingState::Unmanaged:
    return "unmanaged";
  case CityHousingState::Housed:
    return "housed";
  case CityHousingState::Waiting:
    return "waiting";
  }

  return "unknown";
}
} // namespace procedural::world::rewrite
