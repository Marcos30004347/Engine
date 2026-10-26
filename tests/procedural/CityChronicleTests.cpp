// Phase 8 gates of the living city history: direct simulation equals
// checkpoint plus replay, save and load resume with identical hashes,
// scrubbing mutates nothing and leaks no future, lineage stays valid, and
// people, buildings and projects can be inspected and explained.
#include "CityHistoryFixture.hpp"
#include "procedural/world/WorldCityChronicle.hpp"
#include <iostream>
using namespace cityfixture;
using nlohmann::json;

namespace {
void cast(Fixture &f)
{
  for (uint32_t house = 0; house < 4; ++house)
  {
    const uint32_t a = house * 4, b = house * 4 + 1;

    f.person(a, 40 + house, 100, 200 + house, true);
    f.person(b, 36 + house, 100, 200 + house, false);
    f.marry(a, b);
    f.edge("? is head of ?", a, 200 + house);
    f.edge("? swears to ?", 200 + house, 300);
  }

  f.edge("? is king of ?", 0, 300);
  f.person(2, 22, 100, 200, true);
  f.child(2, 0, 1);
  f.person(6, 21, 100, 201, false);
  f.child(6, 4, 5);
  f.person(10, 20, 100, 202, true);
  f.child(10, 8, 9);
  f.person(14, 23, 100, 203, true);
  f.child(14, 12, 13);
}

CityFoundingScript script()
{
  CityFoundingScript s;
  CityFabricSettings site;

  site.base.seed = 41;
  site.base.tiers = 3;
  s.sites[100] = site;

  CityWorkplaceFounding yard;

  yard.week = start;
  yard.spec.settlement = 100;
  yard.spec.occupation = CityOccupation::Labourer;
  yard.spec.slots = 3;
  s.workplaces.push_back(yard);
  s.grants.push_back({start, 100, 0, true, CityGood::Coin, 3000});
  s.grants.push_back({start, 100, 0, false, CityGood::Food, 500});
  s.grants.push_back({start, 100, 0, false, CityGood::Timber, 300});
  s.grants.push_back({start, 100, 0, false, CityGood::Stone, 300});
  s.grants.push_back({start, 100, 0, false, CityGood::Coin, 100});
  s.foundingHouseholdCoin = 60;

  CityProjectIntent home;

  home.kind = CityProjectKind::Home;
  home.settlement = 100;
  home.sponsor = CitySponsorKind::Treasury;
  home.sponsorPerson = 0;
  home.priority = 60;
  home.reason = "a house for the crown";
  s.proposals.push_back({start + 6, home});

  return s;
}

CityPolicies policies()
{
  CityPolicies p;

  p.economy.taxPerMille = 0;
  p.projects.marketHouseholds = 3;
  p.projects.keepHouseholds = 3;

  return p;
}

// Runs the fixture for `weeks`, feeding every chronicle given, with the same
// social changes on the same weeks.
struct Run
{
  Fixture f;
  uint32_t fed = 0;

  Run() : f(CityHouseholdPolicy{})
  {
    cast(f);
  }

  bool step(std::vector<CityChronicle *> chronicles, std::string &error)
  {
    if (fed == 8)
    {
      f.marry(2, 6);
    }

    if (fed == 30)
    {
      f.kill(12);
    }

    if (!f.advance())
    {
      error = f.error;

      return false;
    }

    for (auto *c : chronicles)
    {
      if (!c->observe(*f.snapshot, &error))
      {
        return false;
      }
    }

    ++fed;

    return true;
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

  std::string error;
  CityChronicle direct(policies(), script());
  Run run;
  bool ok = true;

  for (uint32_t w = 0; w < 60 && ok; ++w)
  {
    ok = run.step({&direct}, error);
  }

  if (!ok)
  {
    std::cout << "  " << error << '\n';
  }

  check(ok && direct.state().weeks == 60, "sixty weeks of city history run through the chronicle");

  const auto liveHash = direct.stateHash();
  CityState replayed;

  check(direct.replay({}, replayed, &error) && CityChronicle::hashOf(replayed) == liveHash, "replaying the recorded stream rebuilds the same state, hash for hash");

  CityState partial;

  check(direct.replay(start + 20, partial, &error) && partial.weeks == 21 && CityChronicle::hashOf(partial) != liveHash, "a replay to an earlier week stops there");

  const json saved = direct.save();
  CityChronicle loaded(policies(), script());

  check(CityChronicle::load(saved, loaded, &error) && loaded.stateHash() == liveHash && loaded.stream().size() == 60, "a saved chronicle loads to the same hash");

  // Save at week 30, load, and let both continue with the same story.
  CityChronicle early(policies(), script());
  Run second;

  ok = true;

  for (uint32_t w = 0; w < 30 && ok; ++w)
  {
    ok = second.step({&early}, error);
  }

  CityChronicle resumed(policies(), script());

  check(ok && CityChronicle::load(early.save(), resumed, &error), "a checkpoint at week thirty loads");

  for (uint32_t w = 0; w < 30 && ok; ++w)
  {
    ok = second.step({&early, &resumed}, error);
  }

  check(ok && resumed.stateHash() == early.stateHash() && early.stateHash() == liveHash, "the resumed chronicle and the uninterrupted one reach the same future hash as the direct run");

  // Scrubbing.
  const auto before = direct.stateHash();
  const auto atTen = direct.at(start + 10), atSixty = direct.at(start + 59), beforeAll = direct.at(start - 5);

  check(direct.stateHash() == before, "scrubbing the timeline mutates nothing");

  bool noFuture = true;
  size_t standingAtTen = 0;

  for (const auto &site : atTen.at("sites"))
  {
    for (const auto &b : site.at("buildings"))
    {
      noFuture = noFuture && b.at("fromWeek").get<uint32_t>() <= start + 10;
      ++standingAtTen;
    }
  }

  size_t standingLate = 0;

  for (const auto &site : atSixty.at("sites"))
  {
    standingLate += site.at("buildings").size();
  }

  check(noFuture && standingAtTen > 0 && standingLate > standingAtTen && beforeAll.at("sites").empty(), "no future building leaks into an older date, and nothing stands before the founding");
  check(atTen.at("people").size() == 12 && atSixty.at("people").size() == 11, "the people alive at a date are those the stream recorded");

  bool lineage = true;

  for (const auto &snapshot : direct.stream())
  {
    for (const auto &[id, p] : snapshot.people)
    {
      for (auto parent : p.parents)
      {
        lineage = lineage && snapshot.people.count(parent);
      }
    }
  }

  check(lineage, "every lineage link in the stream resolves to a recorded person");

  // Inspection.
  const auto king = direct.inspectPerson(0);

  check(!king.is_null() && king.at("children").size() == 1 && king.at("offices").size() == 1 && !king.at("home").is_null() && king.at("livelihood").is_object(), "a person shows family, home, work and rank");

  const auto groom = direct.inspectPerson(2);

  check(groom.at("spouse") == 6 && groom.at("parents").size() == 2 && groom.at("household") != king.at("household"), "a married son shows his spouse and his own household");

  const auto &site = direct.state().fabric.sites().at(100);
  const auto building = direct.inspectBuilding(100, site.buildings.begin()->first);

  check(!building.is_null() && !building.at("versions").empty() && !building.at("occupants").empty() && building.at("use") == "home", "a building shows its versions and occupants");

  std::optional<uint64_t> crownHome;

  for (const auto *map : {&direct.state().realm.projects(), &direct.state().realm.closedProjects()})
  {
    for (const auto &[id, p] : *map)
    {
      if (p.intent.reason == "a house for the crown")
      {
        crownHome = id;
      }
    }
  }

  const auto explained = crownHome ? direct.explainProject(*crownHome) : json(nullptr);

  check(!explained.is_null() && explained.at("state") == "completed" && explained.at("timeline").size() >= 3 && explained.at("reason") == "a house for the crown", "a project explains why it was proposed and what happened to it");
  check(direct.series().size() == 2 && direct.series().front().at("settlements").size() == 1, "yearly series rows are kept for charts");
  check(direct.inspectPerson(999).is_null() && direct.explainProject(42).is_null() && direct.inspectBuilding(100, 42).is_null(), "unknown ids inspect to nothing rather than inventing");

  return failures ? 1 : 0;
}
