// Phase 7 gates of the living city history: manor to castle in stages, walls
// in funded segments with gates where the ways cross, closure and connectivity,
// partial and abandoned works, and an outer circuit retiring the inner one.
#include "CityHistoryFixture.hpp"
#include "procedural/world/WorldCityDefence.hpp"
#include <iostream>
#include <sstream>
using namespace cityfixture;

namespace {
struct City
{
  Fixture &f;
  CityEconomyLedger economy;
  CityRealmLedger realm;
  CityFabricLedger &fabric;
  CityDefenceLedger defence;

  City(Fixture &fixture, CityFabricLedger &ground, CityEconomyPolicy policy, CityProjectPolicy projects) : f(fixture), economy(std::move(policy)), realm({}, std::move(projects)), fabric(ground)
  {
  }

  bool advance(uint32_t weeks = 1)
  {
    for (uint32_t i = 0; i < weeks; ++i)
    {
      if (!f.advance() || !economy.observe(*f.snapshot, f.homes, &f.error) || !realm.observe(*f.snapshot, f.homes, economy, &f.error) || !fabric.observe(*f.snapshot, f.homes, economy, realm, &f.error) ||
          !defence.observe(*f.snapshot, f.homes, economy, realm, fabric, &f.error) || !defence.validate(*f.snapshot, fabric, &f.error) || !fabric.validate(*f.snapshot, f.homes, economy, &f.error))
      {
        return false;
      }
    }

    return true;
  }

  const CityDefenceSite &site() const
  {
    return defence.sites().at(100);
  }

  size_t count(CityDefenceEventKind kind) const
  {
    size_t n = 0;

    for (const auto &e : defence.events())
    {
      n += e.kind == kind;
    }

    return n;
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

  uint64_t crownProject(CityProjectKind kind, uint32_t priority, std::optional<std::string> site = {}, const char *reason = "test")
  {
    CityProjectIntent intent;

    intent.kind = kind;
    intent.settlement = 100;
    intent.sponsor = CitySponsorKind::Treasury;
    intent.sponsorPerson = 0;
    intent.priority = priority;
    intent.site = site;
    intent.reason = reason;

    return realm.propose(intent, f.week);
  }

  bool conserved() const
  {
    const auto &t = economy.totals();

    return economy.coinInLedger() == t.coinGranted + t.coinFromTrade - t.coinToTrade - t.coinSpentOutside;
  }
};

std::string serialize(const CityDefenceLedger &defence)
{
  std::ostringstream out;

  for (const auto &e : defence.events())
  {
    out << e.week << ' ' << cityDefenceEventName(e.kind) << ' ' << e.subject << ' ' << e.detail << '\n';
  }

  return out.str();
}

void cast(Fixture &f)
{
  for (uint32_t house = 0; house < 5; ++house)
  {
    const uint32_t a = house * 4, b = house * 4 + 1;

    f.person(a, 40 + house, 100, 200 + house, true);
    f.person(b, 36 + house, 100, 200 + house, false);
    f.marry(a, b);
    f.edge("? is head of ?", a, 200 + house);
    f.edge("? swears to ?", 200 + house, 300);
  }

  f.edge("? is king of ?", 0, 300);
  f.edge("? is queen of ?", 1, 300);
  f.person(2, 22, 100, 200, true);
  f.child(2, 0, 1);
  f.person(6, 21, 100, 201, true);
  f.child(6, 4, 5);
  f.person(10, 20, 100, 202, true);
  f.child(10, 8, 9);
  f.person(14, 23, 100, 203, true);
  f.child(14, 12, 13);
  f.person(18, 24, 100, 204, true);
  f.child(18, 16, 17);
  f.edge("? feuds with ?", 203, 204);
  f.edge("? feuds with ?", 204, 203);
}
} // namespace

int main()
{
  int failures = 0;

  std::cout.setf(std::ios::unitbuf);

  auto check = [&](bool ok, const char *text) {
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
    failures += !ok;
  };

  auto scenario = [&](bool report) {
    CityFabricLedger fabric;
    CityFabricSettings settings;

    settings.base.seed = 41;
    settings.base.tiers = 3;

    std::string error;
    const bool founded = fabric.found(100, settings, start, 5, &error);
    CityHouseholdPolicy housing;

    housing.housingCapacity[100] = fabric.facts(100).homes;

    Fixture f(housing);

    cast(f);

    CityEconomyPolicy policy;

    policy.taxPerMille = 0;

    CityProjectPolicy projects;

    projects.marketHouseholds = 3;
    projects.keepHouseholds = 3;
    projects.wallHouseholds = 30; // The outer circuit is asked for by hand below.
    projects.crownWorksIdle = 1000;   // No crown farms or housing here: the crew is for the defences.
    projects.crownHousingWaiting = 1000;

    City c(f, fabric, policy, projects);
    CityWorkplaceSpec spec;

    spec.settlement = 100;
    spec.occupation = CityOccupation::Labourer;
    spec.slots = 3;
    c.economy.openWorkplace(spec, f.week);
    spec.occupation = CityOccupation::Mason;
    spec.slots = 2;
    c.economy.openWorkplace(spec, f.week);
    c.economy.grantSkill(14, CityOccupation::Mason, 2);
    c.economy.grantSkill(18, CityOccupation::Mason, 2);
    c.economy.grantTreasury(100, 5000);
    c.economy.grantStore(100, CityGood::Timber, 500);
    c.economy.grantStore(100, CityGood::Stone, 800);
    c.economy.grantStore(100, CityGood::Food, 600);
    c.economy.grantStore(100, CityGood::Coin, 200);

    bool ok = founded && c.advance();
    auto trace = [&](const char *where) {
      if (!ok && report)
      {
        std::cout << "  " << where << ": " << f.error << '\n';
      }
    };

    trace("founding");

    if (report)
    {
      check(ok && c.realm.safety(100, *f.snapshot, c.economy, f.homes) < .7, "a feud between two of five houses makes the seat unsafe");
    }

    // The realm asks for a hall, then a palisade, then a keep, in that order.
    ok = ok && c.advance(20);
    trace("manor");

    if (report)
    {
      check(ok && c.project(CityProjectKind::Manor, CityProjectState::Completed) && c.site().stage == CityDefenceStage::Manor, "the crown builds its hall first and the seat becomes a manor");
      check(c.project(CityProjectKind::Palisade) != nullptr, "the unsafe seat asks for a palisade once the hall stands");

      if (!c.project(CityProjectKind::Palisade))
      {
        for (const auto *map : {&c.realm.projects(), &c.realm.closedProjects()})
        {
          for (const auto &[id, p] : *map)
          {
            std::cout << "  project " << cityProjectKindName(p.intent.kind) << ' ' << cityProjectStateName(p.state) << " [" << p.blockedBy << "] " << p.intent.reason << " week " << p.proposedWeek << '\n';
          }
        }

        std::cout << "  safety " << c.realm.safety(100, *f.snapshot, c.economy, f.homes) << " households " << f.homes.livingHouseholds(100) << " treasury " << c.economy.settlements().at(100).treasury << '\n';
      }
    }

    ok = ok && c.advance(40);
    trace("palisade");

    if (report)
    {
      const auto &circuit = c.site().circuits.front();

      check(ok && c.site().circuits.size() == 1 && circuit.palisade && circuit.lifecycle == CityWallLifecycle::Standing && circuit.closed(), "the palisade is built segment by segment and closes");
      check(!circuit.gates.empty() && circuit.gates.front().route == 0, "the approach stair crosses the line, so it has a gate");
      check(circuit.towers.size() >= 4 && c.site().stage == CityDefenceStage::FortifiedManor, "corner towers stand and the manor is fortified");
      check(c.count(CityDefenceEventKind::SegmentCompleted) == circuit.segments.size() && c.conserved(), "every segment was paid for from the escrow, coin conserved");
      check(c.project(CityProjectKind::Keep) != nullptr, "the fortified but unsafe seat asks for a keep");
    }

    ok = ok && c.advance(40);
    trace("keep");

    if (report)
    {
      check(ok && c.project(CityProjectKind::Keep, CityProjectState::Completed) && c.site().stage == CityDefenceStage::Keep, "the keep is raised on the highest terrace the crown can reach");
    }

    // A stone wall replaces the palisade around the same terrace.
    c.crownProject(CityProjectKind::Wall, 80, "wall-1", "stone for the seat");
    ok = ok && c.advance(12);
    trace("wall start");

    // No builders: the works stop and the wall is not reported closed.
    std::vector<uint64_t> yards;

    for (const auto &[id, w] : c.economy.workplaces())
    {
      if (w.occupation == CityOccupation::Labourer || w.occupation == CityOccupation::Mason)
      {
        yards.push_back(id);
      }
    }

    for (auto id : yards)
    {
      c.economy.retireWorkplace(id, f.week);
    }

    ok = ok && c.advance(12);
    trace("no builders");

    if (report)
    {
      const auto &wall = c.site().circuits.back();

      check(ok && c.site().circuits.size() == 2 && !wall.palisade && wall.lifecycle == CityWallLifecycle::Planned && !wall.closed() && c.project(CityProjectKind::Wall)->state == CityProjectState::Suspended, "works without builders are suspended and an incomplete wall is never closed");
      check(c.site().circuits.front().lifecycle == CityWallLifecycle::Standing, "the palisade stands until the wall replaces it");
    }

    spec.occupation = CityOccupation::Labourer;
    spec.slots = 3;
    c.economy.openWorkplace(spec, f.week);
    spec.occupation = CityOccupation::Mason;
    spec.slots = 2;
    c.economy.openWorkplace(spec, f.week);
    ok = ok && c.advance(60);
    trace("wall closes");

    if (report)
    {
      const auto &wall = c.site().circuits.back();

      check(ok && wall.lifecycle == CityWallLifecycle::Standing && wall.closed() && c.site().circuits.front().lifecycle == CityWallLifecycle::Absorbed, "the wall closes and the palisade inside it is retired to a boundary");
      check(c.site().stage == CityDefenceStage::Castle, "a keep inside a towered wall is a castle");
      check(c.project(CityProjectKind::Palisade)->state == CityProjectState::Retired, "the palisade's project is retired, not deleted");
    }

    // Fill the first terrace, open the second and settle it; then an outer circuit.
    for (uint32_t filled = 0; fabric.facts(100).reachableFreeParcels > 0 && filled < 12; ++filled)
    {
      c.crownProject(CityProjectKind::Home, 60, {}, "a house inside the wall");
      ok = ok && c.advance(8);
    }

    c.crownProject(CityProjectKind::Stair, 90, "stair-101", "up to the second terrace");
    ok = ok && c.advance(24);

    for (int i = 0; i < 2; ++i)
    {
      c.crownProject(CityProjectKind::Home, 60, {}, "a house on the second terrace");
      ok = ok && c.advance(8);
    }

    trace("second terrace");

    if (report)
    {
      check(ok && fabric.facts(100).occupiedBlocks.size() == 2, "homes stand on the second terrace, reached by its stair through the wall's gate");
    }

    c.crownProject(CityProjectKind::Wall, 80, "wall-2", "a circuit for the suburb");
    ok = ok && c.advance(8);
    trace("outer planned");

    const auto outerProject = c.project(CityProjectKind::Wall, CityProjectState::Building) ? c.project(CityProjectKind::Wall, CityProjectState::Building)->id : 0;

    if (report)
    {
      check(ok && c.site().circuits.size() == 3 && c.site().circuits.back().enclosedBlocks.size() == 2, "the outer circuit is planned around both terraces");
    }

    // Abandon it: the inner wall keeps standing, the outer stays open.
    c.realm.rejectProject(outerProject, f.week, "the treasury turned to other things", c.economy);
    ok = ok && c.advance(4);
    trace("abandoned");

    if (report)
    {
      check(ok && c.site().circuits.back().abandoned && c.site().circuits.back().lifecycle == CityWallLifecycle::Planned && c.site().circuits[1].lifecycle == CityWallLifecycle::Standing, "an abandoned outer wall stays open and the inner wall keeps standing");
    }

    c.crownProject(CityProjectKind::Wall, 80, "wall-3", "the suburb tries again");
    ok = ok && c.advance(120);
    trace("outer closes");

    if (report)
    {
      const auto &outer = c.site().circuits.back();

      check(ok && c.site().circuits.size() == 4 && outer.lifecycle == CityWallLifecycle::Standing && c.site().circuits[1].lifecycle == CityWallLifecycle::Absorbed, "the second attempt closes and the inner wall is retired only then");

      std::string why;

      check(fabric.reachable(100, c.economy, f.homes, &why), "no way was severed: every home still reaches the court and the exit");

      CityCompletionPlan plan = fabric.completionAt(100, f.week);

      c.defence.applyTo(100, f.week, plan);
      check(!plan.wallSegments.empty() && !plan.gatehouses.empty() && !plan.towers.empty(), "walls, gates and towers are published for meshing");

      CityCompletionPlan early = fabric.completionAt(100, start + 10);

      c.defence.applyTo(100, start + 10, early);
      check(early.wallSegments.empty(), "nothing is published for a date before the first segment stood");
    }

    return serialize(c.defence);
  };

  const auto first = scenario(true);

  check(first == scenario(false), "identical runs produce identical defence events");

  // --- No funds: the wall stays an intention -------------------------------------
  {
    CityFabricLedger fabric;
    CityFabricSettings settings;

    settings.base.seed = 17;
    settings.base.tiers = 2;

    std::string error;

    fabric.found(100, settings, start, 3, &error);

    CityHouseholdPolicy housing;

    housing.housingCapacity[100] = fabric.facts(100).homes;

    Fixture f(housing);

    f.person(0, 40, 100, 200, true);
    f.edge("? is head of ?", 0, 200);
    f.edge("? is king of ?", 0, 300);
    f.person(4, 30, 100, 201, true);
    f.person(8, 30, 100, 202, true);

    City c(f, fabric, {}, {});

    c.economy.grantTreasury(100, 10);
    c.economy.grantStore(100, CityGood::Food, 100);
    c.crownProject(CityProjectKind::Palisade, 90, "palisade", "a fence the crown cannot pay for");
    check(c.advance(8) && c.project(CityProjectKind::Palisade)->state == CityProjectState::Proposed && c.site().circuits.empty(), "an unaffordable wall is not started, and nothing is reported built");
  }

  return failures ? 1 : 0;
}
