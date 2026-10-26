// Working lives: a smith teaches his son, takes a neighbour's boy for free
// work, both graduate journeymen, one is recognised a master, saves, and opens
// a workshop of his own, leaving wage work to run it.
#include "CityHistoryFixture.hpp"
#include "procedural/world/WorldCityDefence.hpp"
#include <iostream>
using namespace cityfixture;

namespace {
struct City
{
  Fixture &f;
  CityEconomyLedger economy;
  CityRealmLedger realm;
  CityFabricLedger &fabric;

  City(Fixture &fixture, CityFabricLedger &ground, CityEconomyPolicy policy) : f(fixture), economy(std::move(policy)), realm(), fabric(ground)
  {
  }

  bool advance(uint32_t weeks = 1)
  {
    for (uint32_t i = 0; i < weeks; ++i)
    {
      if (!f.advance() || !economy.observe(*f.snapshot, f.homes, &f.error) || !realm.observe(*f.snapshot, f.homes, economy, &f.error) || !fabric.observe(*f.snapshot, f.homes, economy, realm, &f.error))
      {
        return false;
      }
    }

    return true;
  }

  const CityLivelihoodRecord &of(HistoryNode id) const
  {
    return economy.livelihoods().at(id);
  }

  size_t count(CityEconomyEventKind kind, std::optional<HistoryNode> person = {}) const
  {
    size_t n = 0;

    for (const auto &e : economy.events())
    {
      n += e.kind == kind && (!person || e.person == person);
    }

    return n;
  }

  std::optional<uint64_t> owned(HistoryNode id) const
  {
    for (const auto &[wid, w] : economy.workplaces())
    {
      if (w.owner == id && !w.retiredWeek)
      {
        return wid;
      }
    }

    return std::nullopt;
  }
};
} // namespace

int main()
{
  int failures = 0;

  std::cout.setf(std::ios::unitbuf);

  auto check = [&](bool ok, const char *text) {
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
    failures += !ok;
  };

  CityFabricLedger fabric;
  CityFabricSettings site;

  site.base.seed = 17;
  site.base.tiers = 3;

  std::string error;

  fabric.found(100, site, start, 3, &error);

  CityHouseholdPolicy housing;

  housing.housingCapacity[100] = fabric.facts(100).homes;

  Fixture f(housing);

  // The smith and his wife, their son of twelve; a neighbour couple with a
  // boy of thirteen and a daughter of ten; a farmer's house.
  f.person(0, 40, 100, 200, true);
  f.person(1, 36, 100, 200, false);
  f.marry(0, 1);
  f.edge("? is head of ?", 0, 200);
  f.person(2, 12, 100, 200, true);
  f.child(2, 0, 1);
  f.person(4, 38, 100, 201, true);
  f.person(5, 35, 100, 201, false);
  f.marry(4, 5);
  f.edge("? is head of ?", 4, 201);
  f.person(6, 13, 100, 201, true);
  f.child(6, 4, 5);
  f.person(7, 10, 100, 201, false);
  f.child(7, 4, 5);
  f.person(8, 45, 100, 202, true);
  f.edge("? is head of ?", 8, 202);
  f.person(9, 19, 100, 202, false);
  f.child(9, 8, 8);

  CityEconomyPolicy policy;

  policy.taxPerMille = 0;
  policy.apprenticesPerMentor = 2;
  policy.tradeOpen = {100};

  City c(f, fabric, policy);
  CityWorkplaceSpec spec;

  spec.settlement = 100;
  spec.occupation = CityOccupation::Smith;
  spec.slots = 2;
  spec.owner = 0;

  const auto smithy = c.economy.openWorkplace(spec, f.week);

  spec.occupation = CityOccupation::Farmer;
  spec.slots = 3;
  spec.owner = 8;
  c.economy.openWorkplace(spec, f.week);
  spec.occupation = CityOccupation::Labourer;
  spec.slots = 3;
  spec.owner.reset();
  c.economy.openWorkplace(spec, f.week);
  c.economy.grantSkill(0, CityOccupation::Smith, 4);
  c.economy.grantStore(100, CityGood::Food, 2000);
  c.economy.grantStore(100, CityGood::Timber, 600);
  c.economy.grantStore(100, CityGood::Stone, 200);
  c.economy.grantStore(100, CityGood::Coin, 400);
  c.economy.grantTreasury(100, 600);

  bool ok = c.advance();

  c.economy.grantHousehold(*f.homes.householdOf(0), CityGood::Coin, 300);
  c.economy.grantHousehold(*f.homes.householdOf(0), CityGood::Timber, 400);
  c.economy.grantHousehold(*f.homes.householdOf(4), CityGood::Coin, 100);
  c.economy.grantHousehold(*f.homes.householdOf(8), CityGood::Coin, 100);
  ok = ok && c.advance();

  if (!ok)
  {
    std::cout << "  " << f.error << '\n';
  }

  check(ok && c.of(2).career == CityCareerStage::Apprentice && c.of(2).mentor == 0u && c.of(2).taughtByParent, "the smith takes his own son as apprentice");
  check(c.of(6).career == CityCareerStage::Apprentice && c.of(6).mentor == 0u && !c.of(6).taughtByParent && c.of(6).trade == CityOccupation::Smith, "the neighbour's boy learns at the same forge, unpaid");
  check(c.economy.workplaces().at(smithy).workers.size() == 1 && c.economy.workplaces().at(smithy).apprentices.size() == 2, "two apprentices bind to one master and take no worker slot");

  ok = ok && c.advance(3);

  size_t toolsBefore = 0;

  for (const auto &e : c.economy.events())
  {
    toolsBefore += e.kind == CityEconomyEventKind::Produced && e.workplace == smithy && e.good == CityGood::Tools ? size_t(e.amount) : 0;
  }

  check(ok && toolsBefore == 6, "the two apprentices' free work counts for one more smith at the forge");

  // Six years on: both boys are men, journeymen of the trade.
  ok = ok && c.advance(6 * 52);

  if (!ok)
  {
    std::cout << "  " << f.error << '\n';
  }

  auto describe = [&](HistoryNode id) {
    const auto &r = c.of(id);

    std::cout << "  person " << id << " adult " << r.adult << " career " << cityCareerStageName(r.career) << " dependency " << cityDependencyName(r.dependency) << " livelihood " << cityLivelihoodName(r.livelihood) << " workplace " << r.workplace.value_or(0)
              << " mentor " << r.mentor.value_or(~0u) << " apprenticed " << r.apprenticedWeek.value_or(0) << " graduated " << r.graduatedWeek.value_or(0) << " skill " << int(r.skills[uint32_t(CityOccupation::Smith)]) << " mentors " << r.mentors.size() << '\n';
  };

  if (c.of(2).career != CityCareerStage::Journeyman)
  {
    describe(2);
    describe(6);
    describe(0);

    for (const auto &e : c.economy.events())
    {
      if ((e.person == 2u || e.person == 6u || e.person == 0u) && e.kind != CityEconomyEventKind::Wages && e.kind != CityEconomyEventKind::ClassChanged)
      {
        std::cout << "  " << e.week << ' ' << cityEconomyEventName(e.kind) << ' ' << e.person.value_or(0) << ' ' << e.workplace << ' ' << e.amount << '\n';
      }
    }
  }

  check(ok && c.of(2).career == CityCareerStage::Journeyman && c.of(2).graduatedWeek && c.of(2).skills[uint32_t(CityOccupation::Smith)] >= 2, "the son graduates a journeyman smith");
  check(c.of(6).career == CityCareerStage::Journeyman && c.count(CityEconomyEventKind::Graduated, 6) == 1, "the neighbour's boy graduates too");
  check(c.of(2).workplace == smithy || c.of(6).workplace == smithy, "a journeyman goes back to his master's forge for wages");
  check(c.of(2).ambition == CityOccupation::Smith && c.count(CityEconomyEventKind::Ambition, 2) == 1, "a journeyman means to keep a forge of his own");

  // Years more: master, and with a house and savings of his own, a forge.
  f.marry(2, 9);
  ok = ok && c.advance(6 * 52);

  if (!ok)
  {
    std::cout << "  " << f.error << '\n';
  }

  const auto home = f.homes.householdOf(2);

  check(ok && home && home != f.homes.householdOf(0), "the son marries and founds his own household");
  check(c.of(2).career == CityCareerStage::Master && c.count(CityEconomyEventKind::Mastered, 2) == 1, "with the years and the skill he is recognised a master");

  c.economy.grantHousehold(*home, CityGood::Coin, 120);
  ok = ok && c.advance(40);

  if (!ok)
  {
    std::cout << "  " << f.error << '\n';
  }

  const auto forge = c.owned(2);

  check(ok && forge && c.of(2).openedWeek && c.count(CityEconomyEventKind::Opened, 2) == 1, "his savings fund a workshop, the fabric raises it, and it opens under him");
  check(forge && c.of(2).workplace == forge && c.of(2).livelihood == CityLivelihood::SelfEmployed && !c.of(2).ambition, "he leaves the wage forge to run his own");

  const auto *project = c.realm.project(forge ? c.economy.workplaces().at(*forge).id : 0);

  (void)project;

  bool fromHisHouse = false;

  for (const auto *map : {&c.realm.projects(), &c.realm.closedProjects()})
  {
    for (const auto &[id, p] : *map)
    {
      fromHisHouse = fromHisHouse || (p.intent.kind == CityProjectKind::Workshop && p.intent.sponsorPerson == 2u && p.intent.household == home && p.state == CityProjectState::Completed);
    }
  }

  check(fromHisHouse, "the workshop was his own household's project, in his name");

  return failures ? 1 : 0;
}
