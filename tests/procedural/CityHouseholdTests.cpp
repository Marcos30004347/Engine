// Phase 2 gates of the living city history: explicit households, guardianship,
// housing and waiting lists, and migration written into the story graph.
#include "CityHistoryFixture.hpp"
#include "procedural/world/WorldCityMigration.hpp"
#include <iostream>
#include <sstream>
using namespace procedural::world::rewrite;
using namespace graphrewrite;

namespace {
using namespace cityfixture;

std::string serialize(const CityHouseholdLedger &homes, const std::vector<CityMigrationEvent> &migration)
{
  std::ostringstream out;

  for (const auto &e : homes.events())
  {
    out << e.week << ' ' << cityHouseholdEventName(e.kind) << ' ' << e.household << ' ' << e.person.value_or(~0u) << ' ' << e.before.value_or(~0u) << ' ' << e.after.value_or(~0u) << ' ' << e.cause << '\n';
  }

  for (const auto &e : migration)
  {
    out << e.week << ' ' << cityMigrationEventName(e.kind) << ' ' << e.household << ' ' << e.origin.value_or(~0u) << ' ' << e.destination.value_or(~0u);

    for (auto p : e.people)
    {
      out << ' ' << p;
    }

    out << '\n';
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

  // --- Households over a multi-generation cast ---------------------------------
  {
    CityHouseholdPolicy policy;

    policy.housingCapacity[100] = 3;

    Fixture f(policy);

    f.person(0, 40, 100, 200, true);
    f.person(1, 36, 100, 200, false);
    f.marry(0, 1);
    f.edge("? is head of ?", 0, 200);
    f.person(2, 18, 100, 200, true);
    f.child(2, 0, 1);
    f.person(3, 8, 100, 200, false);
    f.child(3, 0, 1);
    f.person(4, 45, 100, 201, true);
    f.edge("? is head of ?", 4, 201);
    f.person(5, 20, 100, 201, false);
    f.edge("? is child of ?", 5, 4);
    f.edge("? is father of ?", 4, 5);
    f.person(6, 30, 100, 201, false);
    f.person(7, 42, 100, {}, true);

    check(f.advance(), "founding cast seeds households and passes the gates");
    check(f.liveHouseholds(100) == 3 && f.home(0).id == f.home(3).id && f.home(4).id == f.home(6).id && f.home(7).members.size() == 1, "authored houses seed one household each; the priest lives alone");
    check(f.home(0).head == 0u && f.home(4).head == 4u && f.home(0).kind == CityHouseholdOrigin::Founding, "the oldest working-age member heads a founding household");
    check(f.homes.housedHouseholds(100) == 3 && f.homes.waitingList(100).empty(), "founding households fill the housing stock");

    const auto founders = f.home(0).id;

    f.marry(2, 5);
    check(f.advance() && f.home(2).id == f.home(5).id && f.home(2).id != founders && f.home(2).id != f.home(4).id, "two adult children marrying found a new household");
    check(f.home(2).housing == CityHousingState::Waiting && f.homes.waitingList(100).size() == 1 && f.home(2).kind == CityHouseholdOrigin::LocalFormation, "a new household with no free home waits, and is reported as waiting");

    f.person(8, 0, 100, 200, true);
    f.child(8, 2, 5);
    check(f.advance() && f.home(8).id == f.home(2).id && f.home(2).members.size() == 3, "a newborn joins its parents' household");

    f.kill(0);
    check(f.advance() && f.home(1).id == founders && f.home(1).head == 1u && f.home(1).members.size() == 2 && f.home(1).housing == CityHousingState::Housed, "widowhood keeps the home and passes the headship");

    const auto priests = f.home(7).id;

    f.kill(7);
    check(f.advance() && f.homes.households().at(priests).dissolvedWeek == f.week && f.home(2).housing == CityHousingState::Housed, "death releases the home to the first household waiting");
    check(f.has(CityHouseholdEventKind::HomeReleased, f.week) && f.has(CityHouseholdEventKind::Dissolved, f.week) && f.has(CityHouseholdEventKind::Housed, f.week), "release, extinction and housing are recorded events");

    f.kill(1);
    check(f.advance() && f.home(3).id == f.home(2).id && f.homes.households().at(founders).dissolvedWeek, "an orphaned child is taken in by an adult sibling");
    check(f.snapshot->people.at(3).parents == std::vector<uint32_t>{0, 1} && f.has(CityHouseholdEventKind::GuardianAssigned, f.week), "guardianship never rewrites the parent tree");

    f.moveTo(6, 100, 101);
    check(f.advance() && f.home(6).settlement == 101u && f.home(6).origin == 100u && f.home(6).kind == CityHouseholdOrigin::Migration && f.home(4).members.size() == 1, "one member moving away splits off a household with a known origin");

    const auto young = f.home(2).id;

    for (auto id : {2u, 5u, 8u, 3u})
    {
      f.moveTo(id, 100, 101);
    }

    check(f.advance() && f.home(2).id == young && f.home(2).settlement == 101u && f.home(2).housing == CityHousingState::Unmanaged && f.home(3).id == young, "a household moving together keeps its identity");
    check(f.has(CityHouseholdEventKind::Relocated, f.week) && f.has(CityHouseholdEventKind::HomeReleased, f.week), "relocation releases the old home");

    for (auto id : {2u, 5u, 8u, 3u})
    {
      f.moveTo(id, 101, 100);
    }

    check(f.advance() && f.home(2).id == young && f.home(2).settlement == 100u && f.home(2).housing == CityHousingState::Housed && f.home(2).origin == 101u, "return migrants keep their household and are housed when a home is free");

    const auto wulfings = f.home(4).id;

    f.kill(4);
    check(f.advance() && f.homes.households().at(wulfings).dissolvedWeek == f.week && !f.homes.householdOf(4), "a household dies out when its last member dies");

    auto gap = *f.snapshot;

    gap.week += 2;

    const auto events = f.homes.events().size();

    check(!f.homes.observe(gap, &f.error) && f.homes.events().size() == events && f.homes.week() == f.snapshot->week, "a missing week is rejected without partial household changes");

    std::vector<CityHistoryEvent> none;

    check(!cityPopulationReconciles(*f.snapshot, gap, none, &f.error), "reconciliation rejects non-consecutive weeks");

    auto missing = *f.snapshot;

    ++missing.week;
    missing.people.at(5).alive = false;
    check(!cityPopulationReconciles(*f.snapshot, missing, none, &f.error), "an unrecorded death fails population reconciliation");
  }

  // --- Marriage into a head's household, and independence ----------------------
  {
    CityHouseholdPolicy policy;

    policy.housingCapacity[100] = 2;

    Fixture f(policy);

    f.person(0, 50, 100, 200, true);
    f.person(1, 24, 100, 200, false);
    f.edge("? is child of ?", 1, 0);
    f.person(2, 30, 100, 201, true);
    f.person(3, 26, 100, 201, true);
    f.person(4, 3, 100, 201, false);
    f.edge("? is child of ?", 4, 3);
    f.edge("? is father of ?", 3, 4);
    check(f.advance() && f.liveHouseholds(100) == 2, "second cast seeds two households");

    f.marry(2, 1);
    check(f.advance() && f.home(1).id == f.home(2).id && f.home(0).members.size() == 1, "marrying a head joins the head's household");
    check(f.liveHouseholds(100) == 2 && f.homes.waitingList(100).empty(), "a marriage into an existing home creates no demand");

    CityHouseholdPolicy free;

    free.housingCapacity[100] = 3;
    free.independenceAgeYears = 25;

    CityHouseholdPolicy full;

    full.housingCapacity[100] = 2;
    full.independenceAgeYears = 25;

    CityHouseholdLedger roomy(free), crowded(full);

    check(roomy.observe(*f.snapshot) && crowded.observe(*f.snapshot), "independence ledgers accept the current week");

    auto next = *f.snapshot;

    ++next.week;
    check(roomy.observe(next) && roomy.householdOf(3) != roomy.householdOf(2) && roomy.households().at(*roomy.householdOf(3)).members.count(4), "an adult child founds a home when one is free, taking their own children");
    check(crowded.observe(next) && crowded.householdOf(3) == crowded.householdOf(2), "no free home means the adult child stays: growth is never forced");
  }

  // --- Migration written into the graph ----------------------------------------
  auto migrationScenario = [&](bool report) {
    CityHouseholdPolicy policy;

    policy.housingCapacity[100] = 3;

    Fixture f(policy);

    f.person(0, 40, 100, 200, true);
    f.person(1, 36, 100, 200, false);
    f.marry(0, 1);
    f.edge("? is head of ?", 0, 200);
    f.person(2, 18, 100, 200, true);
    f.child(2, 0, 1);
    f.person(4, 45, 100, 201, true);
    f.edge("? is head of ?", 4, 201);
    f.person(5, 20, 100, 201, false);
    f.person(7, 42, 100, {}, true);

    CityMigrationPolicy rules;

    rules.origins = {"Eoferwic", "Lindcylne"};
    rules.destinations[100] = {1, 1, 1};
    rules.reviewIntervalWeeks = 1;
    rules.householdsPerReview = 1;
    rules.waitingTolerance = 0;
    rules.patienceWeeks = 2;
    rules.maxLivingPeople = 100;
    rules.seed = 7;

    CityMigrationAdapter migration(rules, f.types, f.graph, f.names, base);
    auto publish = [&]() { return migration.publish(f.week, *f.snapshot, f.homes, &f.error); };
    auto count = [&](CityMigrationEventKind kind) {
      size_t n = 0;

      for (const auto &e : migration.events())
      {
        n += e.kind == kind;
      }

      return n;
    };

    if (report)
    {
      check(migration.ready(&f.error) && migration.origins().size() == 2 && f.names.nameOf(migration.origins().begin()->first) == "Eoferwic", "origin settlements become named graph nodes");
    }

    bool ok = f.advance() && publish();

    if (report)
    {
      check(ok && count(CityMigrationEventKind::Generated) == 0, "no home free means nobody is invited: no forced growth");
    }

    f.marry(2, 5);
    ok = ok && f.advance() && publish();

    const auto young = f.home(2).id;

    ok = ok && f.advance() && publish();

    if (report)
    {
      check(ok && count(CityMigrationEventKind::Departed) == 0 && f.home(2).housing == CityHousingState::Waiting, "a waiting household is not pushed out before its patience runs out");
    }

    ok = ok && f.advance() && publish();

    if (report)
    {
      check(ok && count(CityMigrationEventKind::Departed) == 1 && migration.events().back().household == young, "waiting past patience emigrates the household");
    }

    ok = ok && f.advance() && publish();

    if (report)
    {
      check(ok && f.home(2).id == young && migration.isOrigin(*f.home(2).settlement) && f.home(2).origin == 100u, "the emigrant household persists at its named origin");
      check(count(CityMigrationEventKind::Returned) == 0, "no home free means no return yet");
    }

    f.kill(7);
    ok = ok && f.advance() && publish();

    if (report)
    {
      check(ok && count(CityMigrationEventKind::Returned) == 1 && count(CityMigrationEventKind::Generated) == 0, "kin and a freed home bring the emigrants back before strangers are invited");
    }

    ok = ok && f.advance() && publish();

    if (report)
    {
      check(ok && f.home(2).id == young && f.home(2).settlement == 100u && f.home(2).housing == CityHousingState::Housed, "return migrants are the same household, housed again");
    }

    f.kill(4);
    ok = ok && f.advance() && publish();

    if (report)
    {
      check(ok && count(CityMigrationEventKind::Generated) == 1, "a freed home with no returning kin invites a new household");
    }

    ok = ok && f.advance();

    const auto generated = migration.events().back(); // A copy: publish() grows the log.
    bool grouped = ok && !generated.people.empty();

    for (auto id : generated.people)
    {
      grouped = grouped && f.homes.householdOf(id) && f.home(id).settlement == generated.origin && f.home(id).id == f.home(generated.people.front()).id && f.snapshot->people.at(id).alive;
    }

    if (report)
    {
      check(grouped && f.liveHouseholds(*generated.origin) == 1, "a generated family appears at its origin as one household of known people");

      bool ancestry = true;

      for (auto id : generated.people)
      {
        const auto &p = f.snapshot->people.at(id);

        ancestry = ancestry && (p.stage == CityLifeStage::Adult ? p.parents.empty() : p.parents.size() == 2);
      }

      check(ancestry, "generated adults have explicitly unknown ancestry; their children have known parents");
    }

    ok = ok && publish() && f.advance();

    if (report)
    {
      check(ok && count(CityMigrationEventKind::Arrived) == 1 && f.home(generated.people.front()).settlement == 100u && f.home(generated.people.front()).kind == CityHouseholdOrigin::Migration && f.home(generated.people.front()).housing == CityHousingState::Housed,
            "the family moves in as one household and takes the free home");

      compiler::EdgeTypeId peace;

      f.types.find("? is at peace with ?", peace);
      check(f.graph.edgeCount(peace) >= 2, "an arriving house is at peace with the local heads");

      CityMigrationPolicy guard = rules;

      guard.maxLivingPeople = 1;

      CityMigrationAdapter guarded(guard, f.types, f.graph, f.names, base);

      check(guarded.publish(f.week, *f.snapshot, f.homes, &f.error) && !guarded.events().empty() && guarded.events().back().kind == CityMigrationEventKind::Suspended, "the population guard suspends arrivals instead of killing anyone");

      CityMigrationPolicy bleak = rules;

      bleak.destinations[100] = {0, 0, 0};

      CityMigrationAdapter hopeless(bleak, f.types, f.graph, f.names, base);

      check(hopeless.publish(f.week, *f.snapshot, f.homes, &f.error) && hopeless.events().size() == 1 && hopeless.events().back().kind == CityMigrationEventKind::Departed, "no work, food or safety invites nobody and pushes the newest household out");

      CityMigrationPolicy nowhere = rules;

      nowhere.origins.clear();

      CityMigrationAdapter unready(nowhere, f.types, f.graph, f.names, base);

      check(!unready.ready(&f.error), "a destination with no named origin is a fatal policy error");

      compiler::EdgeTypeTable bare;
      runtime::EdgePageStore empty;

      empty.configure(bare);

      CityMigrationAdapter missing(rules, bare, empty, f.names, base);

      check(!missing.ready(&f.error) && f.error.find("schema") != std::string::npos, "missing story schema is a fatal diagnostic");
    }

    if (!ok && report)
    {
      std::cout << "scenario error: " << f.error << '\n';
    }

    return serialize(f.homes, migration.events());
  };

  const auto first = migrationScenario(true);

  check(first == migrationScenario(false), "identical runs produce identical household and migration events");

  return failures ? 1 : 0;
}
