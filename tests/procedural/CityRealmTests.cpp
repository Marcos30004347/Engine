// Phase 4 gates of the living city history: offices read from the graph,
// regency by policy, inheritance that conserves property and coin, and
// projects that are funded once, on one site, or stay unbuilt.
#include "CityHistoryFixture.hpp"
#include "procedural/world/WorldCityRealm.hpp"
#include <iostream>
#include <sstream>
using namespace cityfixture;

namespace {
struct City
{
  Fixture &f;
  CityEconomyLedger economy;
  CityRealmLedger realm;

  City(Fixture &fixture, CityEconomyPolicy policy = {}, CitySuccessionPolicy succession = {}, CityProjectPolicy projects = {}) : f(fixture), economy(std::move(policy)), realm(succession, std::move(projects))
  {
  }

  bool advance(uint32_t weeks = 1)
  {
    for (uint32_t i = 0; i < weeks; ++i)
    {
      if (!f.advance() || !economy.observe(*f.snapshot, f.homes, &f.error) || !realm.observe(*f.snapshot, f.homes, economy, &f.error) || !realm.validate(*f.snapshot, &f.error) || !economy.validate(*f.snapshot, f.homes, &f.error))
      {
        return false;
      }
    }

    return true;
  }

  bool conserved() const
  {
    const auto &t = economy.totals();

    return economy.coinInLedger() == t.coinGranted + t.coinFromTrade - t.coinToTrade - t.coinSpentOutside;
  }

  const CityRealm &crown(HistoryNode realmNode) const
  {
    return realm.realms().at(realmNode);
  }

  size_t count(CityRealmEventKind kind) const
  {
    size_t n = 0;

    for (const auto &e : realm.events())
    {
      n += e.kind == kind;
    }

    return n;
  }

  const CityRealmEvent *last(CityRealmEventKind kind) const
  {
    const CityRealmEvent *found = nullptr;

    for (const auto &e : realm.events())
    {
      if (e.kind == kind)
      {
        found = &e;
      }
    }

    return found;
  }

  const CityProject *project(CityProjectKind kind, std::optional<CityProjectState> state = {}) const
  {
    const CityProject *found = nullptr;

    for (const auto *map : {&realm.projects(), &realm.closedProjects()})
    {
      for (const auto &[id, p] : *map)
      {
        if (p.intent.kind == kind && (!state || p.state == *state))
        {
          found = &p;
        }
      }
    }

    return found;
  }

  int64_t purse(HistoryNode person) const
  {
    const auto home = f.homes.householdOf(person);
    const auto it = home ? economy.accounts().find(*home) : economy.accounts().end();

    return it == economy.accounts().end() ? 0 : it->second.stock[CityGood::Coin];
  }
};

void dump(const City &c)
{
  for (const auto &[id, p] : c.realm.projects())
  {
    std::cout << "  project " << cityProjectKindName(p.intent.kind) << ' ' << cityProjectStateName(p.state) << " priority " << p.intent.priority << " [" << p.blockedBy << "] " << p.intent.reason << '\n';
  }

  for (const auto &[id, t] : c.economy.settlements())
  {
    std::cout << "  town " << id << " treasury " << t.treasury << " store coin " << t.store[CityGood::Coin] << " stone " << t.store[CityGood::Stone] << '\n';
  }

  for (const auto &[id, a] : c.economy.accounts())
  {
    std::cout << "  purse " << id << " coin " << a.stock[CityGood::Coin] << " timber " << a.stock[CityGood::Timber] << '\n';
  }

  std::cout << "  error: " << c.f.error << '\n';
}

std::string serialize(const City &c)
{
  std::ostringstream out;

  for (const auto &e : c.realm.events())
  {
    out << e.week << ' ' << cityRealmEventName(e.kind) << ' ' << e.realm.value_or(~0u) << ' ' << e.person.value_or(~0u) << ' ' << e.before.value_or(~0u) << ' ' << e.after.value_or(~0u) << ' ' << e.asset << ' ' << e.amount << ' ' << e.reason << '\n';
  }

  for (const auto &e : c.economy.events())
  {
    out << e.week << ' ' << cityEconomyEventName(e.kind) << ' ' << e.person.value_or(~0u) << ' ' << e.household << ' ' << e.workplace << ' ' << e.amount << '\n';
  }

  return out.str();
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
    CityHouseholdPolicy housing;

    housing.housingCapacity[100] = 3;

    Fixture f(housing);

    f.person(0, 40, 100, 200, true);
    f.person(1, 36, 100, 200, false);
    f.marry(0, 1);
    f.edge("? is head of ?", 0, 200);
    f.edge("? is king of ?", 0, 300);
    f.edge("? is queen of ?", 1, 300);
    f.edge("? swears to ?", 200, 300);
    f.person(2, 18, 100, 200, true);
    f.child(2, 0, 1);
    f.edge("? is heir to ?", 2, 300);
    f.person(3, 14, 100, 200, false);
    f.child(3, 0, 1);
    f.person(9, 1, 100, 200, true);
    f.child(9, 0, 1);
    f.person(4, 45, 100, 201, true);
    f.edge("? is head of ?", 4, 201);
    f.edge("? swears to ?", 201, 300);
    f.person(5, 20, 100, 201, false);
    f.edge("? is child of ?", 5, 4);
    f.edge("? is father of ?", 4, 5);
    f.person(6, 30, 100, 201, false);
    f.person(7, 42, 100, {}, true);
    f.edge("? serves ?", 7, 310);

    CityEconomyPolicy policy;

    policy.tradeOpen = {100};

    City c(f, policy);
    CityWorkplaceSpec spec;

    spec.settlement = 100;
    spec.occupation = CityOccupation::Farmer;
    spec.slots = 3;
    spec.owner = 0;

    const auto farm = c.economy.openWorkplace(spec, f.week);

    spec.owner.reset();
    spec.occupation = CityOccupation::Merchant;
    spec.slots = 1;
    c.economy.openWorkplace(spec, f.week);
    c.economy.grantSkill(5, CityOccupation::Merchant, 1);
    c.economy.grantTreasury(100, 100);
    c.economy.grantStore(100, CityGood::Coin, 50);
    c.economy.grantStore(100, CityGood::Food, 60);
    c.economy.grantStore(100, CityGood::Stone, 10);

    bool ok = c.advance();

    if (report)
    {
      check(ok && c.crown(300).ruler == 0u && c.crown(300).consort == 1u && c.crown(300).heir == 2u && c.crown(300).authority == CityAuthorityKind::Ruler && c.crown(300).seat == 100u, "the crown, consort and heir are read from the graph");
      check(c.count(CityRealmEventKind::Crowned) == 1 && c.count(CityRealmEventKind::HeirNamed) == 1, "taking office is an event");
    }

    c.economy.grantHousehold(*f.homes.householdOf(7), CityGood::Coin, 40);
    f.marry(2, 5);
    ok = ok && c.advance(4);

    const auto newHome = f.homes.householdOf(2);

    if (report)
    {
      check(ok && c.project(CityProjectKind::Home) && c.project(CityProjectKind::Home)->state == CityProjectState::Proposed && !c.project(CityProjectKind::Home)->blockedBy.empty(), "a household waiting for a home asks for one and stays unbuilt while it cannot pay");
      const bool road = c.project(CityProjectKind::Road) && c.project(CityProjectKind::Road)->state == CityProjectState::Funded && c.project(CityProjectKind::Road)->escrow == c.project(CityProjectKind::Road)->cost;

      check(road, "the crown funds a road for its merchant from the treasury");

      if (!road)
      {
        dump(c);
      }
      check(c.economy.settlements().at(100).store[CityGood::Stone] == 0 && c.conserved(), "the escrow leaves the treasury and the store; coin is conserved");
    }

    c.economy.grantHousehold(*newHome, CityGood::Coin, 50);
    c.economy.grantHousehold(*newHome, CityGood::Timber, 10);
    ok = ok && c.advance(4);

    const int64_t afterFunding = c.purse(2);

    if (report)
    {
      const bool home = ok && c.project(CityProjectKind::Home)->state == CityProjectState::Funded && c.economy.accounts().at(*newHome).stock[CityGood::Timber] == 0;

      check(home, "once affordable the home is funded from the household's own purse");

      if (!home)
      {
        dump(c);
      }
    }

    f.kill(7);
    ok = ok && c.advance();

    if (report)
    {
      check(ok && c.project(CityProjectKind::Home)->state == CityProjectState::Cancelled && c.purse(2) == afterFunding + 20 && c.economy.accounts().at(*newHome).stock[CityGood::Timber] == 10, "a household housed elsewhere has its home project cancelled and refunded");
      check(c.count(CityRealmEventKind::Escheated) == 1 && c.last(CityRealmEventKind::Escheated)->amount > 0 && c.conserved(), "a household dying out with no heir escheats its purse to the treasury");
    }

    f.kill(0);
    f.edge("? wants a king", 300, 300);
    ok = ok && c.advance();

    if (report)
    {
      check(ok && !c.crown(300).ruler && c.crown(300).vacant && c.crown(300).authority == CityAuthorityKind::Regent && c.crown(300).regent == 1u, "a vacant crown has the consort act as regent");
      check(c.economy.workplaces().at(farm).owner == 2u && c.count(CityRealmEventKind::Inherited) == 1 && c.conserved(), "the eldest living child inherits the farm");
    }

    f.edge("? wants a king", 300, 300, false);
    f.edge("? is king of ?", 2, 300);
    ok = ok && c.advance();

    if (report)
    {
      check(ok && c.crown(300).ruler == 2u && c.crown(300).authority == CityAuthorityKind::Ruler && !c.crown(300).regent && c.count(CityRealmEventKind::RegencyEnded) == 1, "an adult heir crowned ends the regency");
    }

    f.kill(2);
    f.edge("? is king of ?", 3, 300);
    ok = ok && c.advance();

    if (report)
    {
      check(ok && c.crown(300).ruler == 3u && c.crown(300).authority == CityAuthorityKind::Regent && c.crown(300).regent == 1u && std::string(c.crown(300).authorityReason).find("minority") == 0, "a child on the throne brings the consort back as regent");
    }

    f.kill(1);
    ok = ok && c.advance();

    if (report)
    {
      check(ok && c.crown(300).regent == 4u && std::string(c.crown(300).authorityReason).find("elected") != std::string::npos, "with no consort and no adult of the house, the largest household's head is elected");
    }

    f.edge("? claims ?", 6, 300);
    ok = ok && c.advance();

    if (report)
    {
      check(ok && c.crown(300).claimants.count(6) && c.count(CityRealmEventKind::ClaimRaised) == 1, "a pretender's claim is a distinct office");
    }

    f.kill(6);
    ok = ok && c.advance();

    if (report)
    {
      check(ok && c.crown(300).claimants.empty() && c.count(CityRealmEventKind::ClaimDropped) == 1, "no dead claimant keeps a claim");
    }

    return serialize(c);
  };

  const auto first = scenario(true);

  check(first == scenario(false), "identical runs produce identical realm and economy events");

  // --- Nobody can act ------------------------------------------------------------
  {
    Fixture f({});

    f.person(20, 10, 105, 250, true);
    f.edge("? is king of ?", 20, 301);

    City c(f);

    check(c.advance() && c.crown(301).authority == CityAuthorityKind::None && c.count(CityRealmEventKind::NoAuthority) == 1, "a child king with nobody to act for him is recorded as no authority");
    check(c.advance(2) && c.count(CityRealmEventKind::NoAuthority) == 1 && c.crown(301).authority == CityAuthorityKind::None, "the no-authority case terminates and does not repeat");
  }

  // --- One site, one treasury -----------------------------------------------------
  {
    Fixture f({});

    f.person(0, 30, 105, 250, true);

    City c(f);

    c.economy.grantTreasury(105, 20);
    check(c.advance(), "a settlement with a purse but no realm seeds");

    CityProjectIntent intent;

    intent.kind = CityProjectKind::Farm;
    intent.settlement = 105;
    intent.sponsor = CitySponsorKind::Treasury;
    intent.site = "field";
    intent.priority = 80;
    intent.reason = "second";

    const auto second = c.realm.propose(intent, f.week);

    intent.priority = 90;
    intent.reason = "first";

    const auto firstId = c.realm.propose(intent, f.week);

    intent.site.reset();
    intent.priority = 70;
    intent.reason = "third";

    const auto third = c.realm.propose(intent, f.week);

    check(c.advance(4), "the review resolves the proposals");

    const auto *one = c.realm.project(firstId), *two = c.realm.project(second), *three = c.realm.project(third);

    check(one && two && one->state == CityProjectState::Funded && two->state == CityProjectState::Cancelled && two->blockedBy == "site taken", "the higher priority takes the site; the other is cancelled, whatever order it was proposed in");
    check(three && three->state == CityProjectState::Proposed && three->blockedBy.find("unaffordable") == 0 && c.economy.settlements().at(105).treasury == 5, "the treasury is spent once; the next intent waits with its reason");
    check(c.conserved(), "coin is conserved through escrow");
  }

  return failures ? 1 : 0;
}
