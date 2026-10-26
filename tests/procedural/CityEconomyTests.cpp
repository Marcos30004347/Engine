// Phase 3 gates of the living city history: livelihoods, workplaces with finite
// slots, apprenticeship, commuting, monthly budgets, and balanced stocks.
#include "CityHistoryFixture.hpp"
#include "procedural/world/WorldCityEconomy.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
using namespace cityfixture;

namespace {
struct Economy
{
  Fixture &f;
  CityEconomyLedger ledger;
  int64_t initialFood = 0;
  bool seeded = false;

  Economy(Fixture &fixture, CityEconomyPolicy policy) : f(fixture), ledger(std::move(policy))
  {
  }

  int64_t food() const
  {
    int64_t total = 0;

    for (const auto &[id, purse] : ledger.accounts())
    {
      total += purse.stock[CityGood::Food];
    }

    for (const auto &[id, town] : ledger.settlements())
    {
      total += town.store[CityGood::Food];
    }

    return total;
  }

  bool advance(uint32_t weeks = 1)
  {
    for (uint32_t i = 0; i < weeks; ++i)
    {
      if (!f.advance() || !ledger.observe(*f.snapshot, f.homes, &f.error) || !ledger.validate(*f.snapshot, f.homes, &f.error))
      {
        return false;
      }

      if (!seeded)
      {
        initialFood = food();
        seeded = true;
      }
    }

    return true;
  }

  bool balanced() const
  {
    const auto &t = ledger.totals();
    const bool foodOk = food() == initialFood + t.foodProduced - t.foodConsumed - t.foodExported + t.foodImported;
    const bool coinOk = ledger.coinInLedger() == t.coinGranted + t.coinFromTrade - t.coinToTrade - t.coinSpentOutside;

    return foodOk && coinOk;
  }

  size_t count(CityEconomyEventKind kind, std::optional<CityGood> good = {}) const
  {
    size_t n = 0;

    for (const auto &e : ledger.events())
    {
      n += e.kind == kind && (!good || e.good == *good);
    }

    return n;
  }

  const CityLivelihoodRecord &of(HistoryNode id) const
  {
    return ledger.livelihoods().at(id);
  }
};

std::string serialize(const CityEconomyLedger &ledger)
{
  std::ostringstream out;

  for (const auto &e : ledger.events())
  {
    out << e.week << ' ' << cityEconomyEventName(e.kind) << ' ' << e.person.value_or(~0u) << ' ' << e.household << ' ' << e.workplace << ' ' << cityGoodName(e.good) << ' ' << e.amount << '\n';
  }

  return out.str();
}

// The cast every scenario starts from: two houses, a priest, a farmer abroad.
void cast(Fixture &f)
{
  f.person(0, 40, 100, 200, true);
  f.person(1, 36, 100, 200, false);
  f.marry(0, 1);
  f.edge("? is head of ?", 0, 200);
  f.person(2, 18, 100, 200, true);
  f.child(2, 0, 1);
  f.person(3, 14, 100, 200, false);
  f.child(3, 0, 1);
  f.person(9, 1, 100, 200, true);
  f.child(9, 0, 1);
  f.person(4, 45, 100, 201, true);
  f.edge("? is head of ?", 4, 201);
  f.person(5, 20, 100, 201, false);
  f.edge("? is child of ?", 5, 4);
  f.person(6, 30, 100, 201, false);
  f.person(7, 42, 100, {}, true);
  f.edge("? serves ?", 7, 300);
  f.person(8, 30, 101, 202, true);
  f.person(10, 28, 100, 203, true);
}

CityHouseholdPolicy roomy()
{
  CityHouseholdPolicy policy;

  policy.housingCapacity[100] = 10;

  return policy;
}

struct Places
{
  uint64_t farm = 0, timber = 0, smithy = 0, church = 0, market = 0, watch = 0, farAway = 0, quarry = 0;
};

Places workplaces(CityEconomyLedger &ledger, uint32_t week)
{
  Places p;
  CityWorkplaceSpec spec;

  spec.settlement = 100;
  spec.occupation = CityOccupation::Farmer;
  spec.slots = 3;
  spec.owner = 0;
  p.farm = ledger.openWorkplace(spec, week);
  spec.owner.reset();
  spec.occupation = CityOccupation::Labourer;
  spec.slots = 2;
  p.timber = ledger.openWorkplace(spec, week);
  spec.occupation = CityOccupation::Smith;
  spec.slots = 1;
  p.smithy = ledger.openWorkplace(spec, week);
  spec.occupation = CityOccupation::Cleric;
  p.church = ledger.openWorkplace(spec, week);
  spec.occupation = CityOccupation::Merchant;
  p.market = ledger.openWorkplace(spec, week);
  spec.occupation = CityOccupation::Guard;
  p.watch = ledger.openWorkplace(spec, week);
  spec.occupation = CityOccupation::Mason;
  p.quarry = ledger.openWorkplace(spec, week);
  spec.settlement = 101;
  spec.occupation = CityOccupation::Farmer;
  spec.slots = 2;
  p.farAway = ledger.openWorkplace(spec, week);

  return p;
}

void grants(CityEconomyLedger &ledger)
{
  ledger.grantStore(100, CityGood::Food, 60);
  ledger.grantStore(100, CityGood::Coin, 50);
  ledger.grantTreasury(100, 40);
  ledger.grantStore(101, CityGood::Food, 20);
  ledger.grantTreasury(101, 10);
}
} // namespace

int main()
{
  int failures = 0;
  auto check = [&](bool ok, const char *text) {
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
    failures += !ok;
  };

  auto scenario = [&](bool report) {
    Fixture f(roomy());

    cast(f);

    CityEconomyPolicy policy;

    policy.tradeOpen = {100};
    policy.taxPerMille = 250;
    policy.seed = 3;

    Economy e(f, policy);
    const auto p = workplaces(e.ledger, f.week);

    grants(e.ledger);
    e.ledger.grantSkill(4, CityOccupation::Labourer, 3);
    e.ledger.grantSkill(6, CityOccupation::Smith, 2);
    e.ledger.grantSkill(5, CityOccupation::Merchant, 1);
    e.ledger.grantSkill(10, CityOccupation::Mason, 1);

    bool ok = e.advance();

    if (report)
    {
      check(ok, "the founding week allocates work and passes the economy gates");
      check(e.of(0).livelihood == CityLivelihood::SelfEmployed && e.of(2).livelihood == CityLivelihood::SelfEmployed && e.of(0).workplace == p.farm, "an owner's household works its own farm");
      check(e.of(1).livelihood == CityLivelihood::Caregiver && !e.of(1).workplace, "a parent of an infant in a two-adult home is the caregiver");
      check(e.of(7).occupation == CityOccupation::Cleric && e.ledger.workplaces().at(p.church).workers.size() == 1, "the priest alone may serve the church");
      check(e.of(6).workplace == p.smithy && e.of(4).workplace == p.timber && e.of(5).workplace == p.market, "skills decide who takes the skilled slots");
      check(e.of(3).dependency == CityDependency::Apprentice && e.of(3).mentor == 4u && e.ledger.workplaces().at(p.timber).workers.size() == 1, "an apprentice-age child binds to a master and takes no worker slot");
      check(e.ledger.workplaces().at(p.farAway).workers.size() == 1 && e.of(8).workplace == p.farAway, "nobody commutes without a route; the local farmer works his own field");
      check(e.of(9).dependency == CityDependency::Dependent && !e.of(9).workplace, "a child is a dependent");

      const auto facts = e.ledger.facts(100, f.homes);

      check(facts.employed == 7 && facts.unemployed == 0 && facts.freeSlots == 3 && facts.work == 1.0, "facts count the employed, the idle and the free slots");
    }

    ok = ok && e.advance(3);

    if (report)
    {
      check(ok && e.count(CityEconomyEventKind::Produced) == 0, "nothing is produced before the first month closes");
    }

    ok = ok && e.advance();

    if (report)
    {
      bool harvest = false;

      for (const auto &ev : e.ledger.events())
      {
        harvest = harvest || (ev.kind == CityEconomyEventKind::Produced && ev.workplace == p.farm && ev.household == *f.homes.householdOf(0) && ev.amount == 32);
      }

      check(ok && e.count(CityEconomyEventKind::Produced, CityGood::Food) == 2 && harvest, "the first month's harvest lands in the owner's store");
      check(e.count(CityEconomyEventKind::Produced, CityGood::Tools) > 0 && e.count(CityEconomyEventKind::Produced, CityGood::Stone) > 0, "the smith works the store's timber and the mason cuts with one of the tools");
      check(e.count(CityEconomyEventKind::Wages) > 0 && e.count(CityEconomyEventKind::Tax) > 0 && e.count(CityEconomyEventKind::Bought) > 0, "wages, taxes and food purchases flow monthly");
      check(e.balanced(), "food and coin balance after the first month");
    }

    ok = ok && e.advance(8);

    if (report)
    {
      check(ok && e.balanced(), "food and coin still balance after three months");
      check(e.count(CityEconomyEventKind::Exported) > 0, "a merchant exports surplus where trade is open");
    }

    f.kill(4);
    ok = ok && e.advance();

    if (report)
    {
      const auto &timber = e.ledger.workplaces().at(p.timber);

      check(ok && e.of(3).mentor != 4u && !e.ledger.livelihoods().count(4) && std::find(timber.workers.begin(), timber.workers.end(), 4u) == timber.workers.end(), "a master's death releases the apprentice and the slot");
      check(e.of(3).dependency == CityDependency::Apprentice && e.of(3).mentor == 6u, "the orphaned apprentice is re-bound to the next master with the skill");
    }

    e.ledger.markUnableToWork(2, true);
    ok = ok && e.advance();

    if (report)
    {
      check(ok && e.of(2).livelihood == CityLivelihood::UnableToWork && !e.of(2).workplace, "an adult marked unable leaves work explicitly");
    }

    for (uint32_t id = 20; id < 50; ++id)
    {
      f.person(id, 25, 100, 200 + id, id % 2 == 0);
    }

    e.ledger.grantSkill(21, CityOccupation::Mason, 1);
    e.ledger.grantHousehold(*f.homes.householdOf(0), CityGood::Coin, 200); // The farm's owner can pay a hand.
    ok = ok && e.advance();

    if (report)
    {
      const auto facts = e.ledger.facts(100, f.homes);

      check(ok && facts.unemployed > 0 && facts.freeSlots == 0 && facts.work < 1.0, "a large immigration fills every slot and leaves the rest unemployed");
    }

    ok = ok && e.advance(4);

    if (report)
    {
      check(ok && e.balanced(), "food and coin balance under a crowd");
    }

    return serialize(e.ledger);
  };

  const auto first = scenario(true);

  check(first == scenario(false), "identical runs produce identical economy events");

  // --- Routes let neighbours commute ------------------------------------------
  {
    Fixture f(roomy());

    cast(f);

    CityEconomyPolicy policy;

    policy.routes.insert({100, 101});

    Economy e(f, policy);
    const auto p = workplaces(e.ledger, f.week);

    e.ledger.grantSkill(4, CityOccupation::Labourer, 3);
    e.ledger.grantSkill(6, CityOccupation::Smith, 2);
    e.ledger.grantSkill(5, CityOccupation::Merchant, 1);
    grants(e.ledger);

    for (uint32_t id = 11; id < 15; ++id)
    {
      f.person(id, 22, 100, 210 + id, true);
    }

    check(e.advance() && e.ledger.workplaces().at(p.farAway).workers.size() == 2, "once local slots are full, a route lets a neighbour's field be worked");
    check(e.count(CityEconomyEventKind::Exported) == 0 && e.advance(4) && e.count(CityEconomyEventKind::Exported) == 0, "with trade closed the merchant exports nothing");
  }

  // --- Harvest failure and hunger -----------------------------------------------
  {
    Fixture f(roomy());

    cast(f);

    CityEconomyPolicy policy;

    policy.harvestPerMille[0] = 0;

    Economy e(f, policy);

    workplaces(e.ledger, f.week);
    e.ledger.grantStore(100, CityGood::Food, 30);
    e.ledger.grantStore(100, CityGood::Coin, 10);
    e.ledger.grantTreasury(100, 20);
    check(e.advance(13) && e.count(CityEconomyEventKind::Produced, CityGood::Food) == 0, "a failed harvest yields nothing");
    check(e.count(CityEconomyEventKind::Subsistence) > 0 && e.count(CityEconomyEventKind::Hunger) > 0 && e.balanced(), "an empty granary gives rations, then explicit hunger, never negative food");

    const auto facts = e.ledger.facts(100, f.homes);

    check(facts.food == 0.0 && facts.foodStock == 0, "the food prospect reads empty");
  }

  // --- No labour ---------------------------------------------------------------
  {
    CityHouseholdPolicy policy;
    Fixture f(policy);

    f.person(0, 8, 100, 200, true);

    Economy e(f, {});

    workplaces(e.ledger, f.week);
    e.ledger.grantStore(100, CityGood::Food, 8);
    check(e.advance(5) && e.count(CityEconomyEventKind::Produced) == 0 && e.count(CityEconomyEventKind::Hired) == 0 && e.balanced(), "a settlement of one child produces nothing and stays consistent");
  }

  // --- No stone without tools -----------------------------------------------------
  {
    Fixture f(roomy());

    f.person(0, 30, 100, 200, true);
    f.edge("? is head of ?", 0, 200);

    Economy e(f, {});
    CityWorkplaceSpec spec;

    spec.settlement = 100;
    spec.occupation = CityOccupation::Mason;
    e.ledger.grantSkill(0, CityOccupation::Mason, 1);
    e.ledger.openWorkplace(spec, f.week);
    e.ledger.grantStore(100, CityGood::Food, 20);
    check(e.advance(5) && e.of(0).occupation == CityOccupation::Mason && e.count(CityEconomyEventKind::Produced, CityGood::Stone) == 0 && e.ledger.settlements().at(100).store[CityGood::Tools] == 0 && e.balanced(),
          "a mason with no tools cuts no stone and borrows none");
  }

  // --- Retirement --------------------------------------------------------------
  {
    Fixture f(roomy());

    f.person(0, 64, 100, 200, true);
    f.edge("? is head of ?", 0, 200);

    Economy e(f, {});
    CityWorkplaceSpec spec;

    spec.settlement = 100;
    spec.occupation = CityOccupation::Labourer;
    e.ledger.openWorkplace(spec, f.week);
    check(e.advance() && e.of(0).livelihood == CityLivelihood::Employed, "a man of sixty-four still works");
    check(e.advance(52) && e.of(0).livelihood == CityLivelihood::Retired && !e.of(0).workplace, "at sixty-five he retires and frees the slot");
  }

  return failures ? 1 : 0;
}
