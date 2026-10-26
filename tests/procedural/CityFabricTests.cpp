// Phase 5 and 6 gates of the living city history: persistent parcels,
// building versions, construction from funded projects, tenancy and
// relocation, terrace access, routes, traffic and reachability.
#include "CityHistoryFixture.hpp"
#include "procedural/world/WorldCityFabric.hpp"
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

  City(Fixture &fixture, CityFabricLedger &ground, CityEconomyPolicy policy = {}) : f(fixture), economy(std::move(policy)), realm(), fabric(ground)
  {
  }

  bool advance(uint32_t weeks = 1)
  {
    for (uint32_t i = 0; i < weeks; ++i)
    {
      if (!f.advance() || !economy.observe(*f.snapshot, f.homes, &f.error) || !realm.observe(*f.snapshot, f.homes, economy, &f.error) || !fabric.observe(*f.snapshot, f.homes, economy, realm, &f.error) ||
          !fabric.validate(*f.snapshot, f.homes, economy, &f.error))
      {
        return false;
      }
    }

    return true;
  }

  const CityFabricSite &site() const
  {
    return fabric.sites().at(100);
  }

  size_t count(CityFabricEventKind kind) const
  {
    size_t n = 0;

    for (const auto &e : fabric.events())
    {
      n += e.kind == kind;
    }

    return n;
  }

  const CityFabricEvent *last(CityFabricEventKind kind) const
  {
    const CityFabricEvent *found = nullptr;

    for (const auto &e : fabric.events())
    {
      if (e.kind == kind)
      {
        found = &e;
      }
    }

    return found;
  }

  bool conserved() const
  {
    const auto &t = economy.totals();

    return economy.coinInLedger() == t.coinGranted + t.coinFromTrade - t.coinToTrade - t.coinSpentOutside;
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
    intent.priority = priority;
    intent.site = site;
    intent.reason = reason;

    return realm.propose(intent, f.week);
  }
};

std::string serialize(const CityFabricLedger &fabric)
{
  std::ostringstream out;

  for (const auto &e : fabric.events())
  {
    out << e.week << ' ' << cityFabricEventName(e.kind) << ' ' << e.subject << ' ' << e.household << ' ' << e.detail << '\n';
  }

  return out.str();
}

CityFabricSettings hillside()
{
  CityFabricSettings settings;

  settings.base.seed = 17;
  settings.base.tiers = 3;

  return settings;
}

void cast(Fixture &f)
{
  // Five houses, each a couple; three grown sons who will be the crew.
  for (uint32_t house = 0; house < 5; ++house)
  {
    const uint32_t a = house * 4, b = house * 4 + 1;

    f.person(a, 40 + house, 100, 200 + house, true);
    f.person(b, 36 + house, 100, 200 + house, false);
    f.marry(a, b);
    f.edge("? is head of ?", a, 200 + house);
  }

  f.person(2, 22, 100, 200, true);
  f.child(2, 0, 1);
  f.person(6, 21, 100, 201, false);
  f.child(6, 4, 5);
  f.person(10, 20, 100, 202, true);
  f.child(10, 8, 9);
  f.person(3, 19, 100, 200, false);
  f.child(3, 0, 1);
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
    std::string error;
    const bool founded = fabric.found(100, hillside(), start, 5, &error);

    if (report)
    {
      check(founded, "a terraced base is planned and the founding homes stand on the first terrace");
    }

    CityHouseholdPolicy housing;

    housing.housingCapacity[100] = fabric.facts(100).homes;

    Fixture f(housing);

    cast(f);

    CityEconomyPolicy policy;

    policy.taxPerMille = 0;

    City c(f, fabric, policy);
    CityWorkplaceSpec yard;

    yard.settlement = 100;
    yard.occupation = CityOccupation::Labourer;
    yard.slots = 3;

    const auto yardId = c.economy.openWorkplace(yard, f.week);

    c.economy.grantTreasury(100, 2000);
    c.economy.grantStore(100, CityGood::Timber, 300);
    c.economy.grantStore(100, CityGood::Stone, 300);
    c.economy.grantStore(100, CityGood::Food, 400);
    c.economy.grantStore(100, CityGood::Coin, 200);

    bool ok = c.advance();
    std::map<uint64_t, uint64_t> foundingHashes;

    auto trace = [&](const char *where) {
      if (!ok && report)
      {
        std::cout << "  " << where << ": " << f.error << '\n';
      }
    };

    trace("founding week");

    for (const auto &[id, b] : c.site().buildings)
    {
      foundingHashes[id] = b.versions.front().geometryHash;
    }

    if (report)
    {
      const auto facts = fabric.facts(100);

      check(ok && facts.homes == 5 && facts.occupiedHomes == 5 && facts.vacantHomes == 0, "five founding households are housed in five founding homes");
      check(facts.occupiedBlocks.size() == 1 && c.site().structures.size() >= 6, "only the first terrace is occupied; the stairs above are planned, not built");
      check(c.economy.workplaces().at(yardId).workers.size() == 3, "three labourers form the crew");

      CityFabricLedger again;

      again.found(100, hillside(), start, 5, &error);

      bool same = true;

      for (const auto &[id, b] : again.sites().at(100).buildings)
      {
        same = same && foundingHashes.at(id) == b.versions.front().geometryHash;
      }

      check(same, "the same settings give the same parcels and geometry hashes");
    }

    // A sixth household: two adult children of different houses marry and wait for a home.
    f.marry(2, 6);
    ok = ok && c.advance();

    const auto newcomers = f.homes.householdOf(2);

    c.economy.grantHousehold(*newcomers, CityGood::Coin, 60);
    c.economy.grantHousehold(*newcomers, CityGood::Timber, 20);
    ok = ok && c.advance(3);
    trace("first review");

    if (report)
    {
      const auto *home = c.project(CityProjectKind::Home);

      const bool started = ok && home && home->state == CityProjectState::Building && c.count(CityFabricEventKind::ConstructionStarted) == 1;

      check(started, "a funded home starts on a free parcel of the first terrace");

      if (!started)
      {
        std::cout << "  home state " << (home ? cityProjectStateName(home->state) : "none") << " started events " << c.count(CityFabricEventKind::ConstructionStarted) << '\n';

        for (const auto &e : fabric.events())
        {
          std::cout << "  " << e.week << ' ' << cityFabricEventName(e.kind) << ' ' << e.subject << ' ' << e.detail << '\n';
        }
      }
      check(home && home->escrow[CityGood::Timber] == 0 && home->spent[CityGood::Timber] == 10, "materials leave the escrow when the works start");

      if (!home)
      {
        for (const auto &[hid, h] : f.homes.households())
        {
          std::cout << "  household " << hid << " head " << h.head.value_or(~0u) << ' ' << cityHousingStateName(h.housing) << " members " << h.members.size() << '\n';
        }

        for (const auto &[id, p] : c.realm.projects())
        {
          std::cout << "  project " << cityProjectKindName(p.intent.kind) << ' ' << cityProjectStateName(p.state) << ' ' << p.blockedBy << '\n';
        }

        std::cout << "  realm week " << c.realm.week().value_or(0) << " fixture week " << f.week << " events " << c.realm.events().size() << '\n';
      }
    }

    ok = ok && c.advance(4);
    trace("first month of works");

    if (report)
    {
      const auto facts = fabric.facts(100);
      const auto *home = c.project(CityProjectKind::Home);

      check(ok && home && c.count(CityFabricEventKind::ConstructionCompleted) == 1 && facts.homes == 6 && home->state == CityProjectState::Completed, "one month of three builders finishes the home");
    }

    ok = ok && c.advance();

    if (report)
    {
      const auto facts = fabric.facts(100);
      const auto *home = c.project(CityProjectKind::Home);

      check(ok && facts.occupiedHomes == 6 && f.homes.households().at(*newcomers).housing == CityHousingState::Housed, "the new home is published and the waiting household moves in the week after");
      check(c.conserved() && home && home->escrow == CityStock{}, "coin paid to the crew comes back into the ledger and the escrow is empty");

      const auto &newest = c.site().buildings.at(c.site().buildings.rbegin()->first);

      check(newest.builders.size() >= 1 && newest.sponsorHousehold == newcomers, "the building records its sponsor and its builders");

      bool untouched = true;

      for (const auto &[id, hash] : foundingHashes)
      {
        untouched = untouched && c.site().buildings.at(id).versions.front().geometryHash == hash && c.site().buildings.at(id).versions.size() == 1;
      }

      check(untouched, "untouched buildings keep their ids, footprints and hashes");
    }

    // Fill the first terrace with crown homes until the stair is needed.
    uint32_t crownHomes = 0;

    while (fabric.facts(100).reachableFreeParcels > 0 && crownHomes < 12)
    {
      c.crownProject(CityProjectKind::Home, 10, {}, "a house for the crown");
      ++crownHomes;
      ok = ok && c.advance(4);
    }

    c.crownProject(CityProjectKind::Home, 10, {}, "a house beyond the first terrace");
    ok = ok && c.advance(4);

    if (report)
    {
      check(ok && fabric.facts(100).reachableFreeParcels == 0 && c.last(CityFabricEventKind::Waiting) && c.last(CityFabricEventKind::Waiting)->detail.find("stair") != std::string::npos, "with the first terrace full a home waits for the stair");
      check(c.project(CityProjectKind::Stair) != nullptr, "the fabric asks the crown for the stair");
    }

    ok = ok && c.advance(24);
    trace("stair");

    if (report)
    {
      check(ok && c.count(CityFabricEventKind::AccessCompleted) == 1 && fabric.facts(100).occupiedBlocks.size() == 2, "the funded stair is built and the second terrace opens");

      std::string why;

      check(fabric.reachable(100, c.economy, f.homes, &why), "every home reaches the court and the exit, the second terrace by its stair");
    }

    // A rebuild needs somewhere to put the household first.
    const uint64_t first = c.site().buildings.begin()->first;
    const auto tenant = c.site().buildings.at(first).tenant;
    const auto beforeRebuild = f.week;

    for (const auto &[id, b] : c.site().buildings)
    {
      (void)id;
      (void)b;
    }

    bool vacancy = fabric.facts(100).vacantHomes > 0;

    if (!vacancy)
    {
      c.crownProject(CityProjectKind::Home, 10, {}, "a spare house");
      ok = ok && c.advance(8);
      vacancy = fabric.facts(100).vacantHomes > 0;
    }

    c.crownProject(CityProjectKind::Rebuild, 60, "building-" + std::to_string(first), "rebuild the founder's house");
    ok = ok && c.advance(16);
    trace("rebuild");

    if (report)
    {
      const auto &b = c.site().buildings.at(first);

      const bool rebuilt = ok && vacancy && c.count(CityFabricEventKind::Relocated) >= 2 && b.versions.size() == 2 && b.versions[0].toWeek && b.versions[0].toWeek == b.versions[1].fromWeek && b.tenant == tenant;

      check(rebuilt, "the tenant is relocated, the house rebuilt in one step, and the tenant returns");

      if (!rebuilt)
      {
        std::cout << "  vacancy " << vacancy << " relocated " << c.count(CityFabricEventKind::Relocated) << " versions " << b.versions.size() << " tenant " << b.tenant.value_or(0) << " expected " << tenant.value_or(0) << '\n';

        for (const auto &[id, p] : c.realm.projects())
        {
          if (p.intent.kind == CityProjectKind::Rebuild)
          {
            std::cout << "  rebuild " << cityProjectStateName(p.state) << ' ' << p.blockedBy << '\n';
          }
        }

        for (const auto &e : fabric.events())
        {
          if (e.week >= beforeRebuild)
          {
            std::cout << "  " << e.week << ' ' << cityFabricEventName(e.kind) << ' ' << e.subject << ' ' << e.detail << '\n';
          }
        }
      }

      bool oneVersion = true;

      for (uint32_t w = beforeRebuild; w <= f.week; ++w)
      {
        uint32_t active = 0;

        for (const auto &v : b.versions)
        {
          active += v.fromWeek <= w && (!v.toWeek || w < *v.toWeek);
        }

        oneVersion = oneVersion && active == 1;
      }

      check(oneVersion && b.versions[0].geometryHash != b.versions[1].geometryHash, "exactly one version stands at every date and the rebuild changed the geometry");
      check(fabric.standingAt(100, start).size() == 5 && fabric.standingAt(100, start - 1).empty() && fabric.standingAt(100, f.week).size() > 6, "the timeline shows what stood at each date and nothing before the founding");
    }

    // Suspension without a crew, and resumption.
    c.crownProject(CityProjectKind::Keep, 70, "keep", "a keep on the high terrace");
    ok = ok && c.advance(4);
    c.economy.retireWorkplace(yardId, f.week);
    ok = ok && c.advance(8);
    trace("suspension");

    if (report)
    {
      check(ok && c.project(CityProjectKind::Keep) && c.project(CityProjectKind::Keep)->state == CityProjectState::Suspended, "works with no builders are suspended, not finished");
    }

    const auto yard2 = c.economy.openWorkplace(yard, f.week);

    (void)yard2;
    ok = ok && c.advance(4);

    if (report)
    {
      const auto *keep = c.project(CityProjectKind::Keep);

      check(ok && keep && keep->state == CityProjectState::Building && keep->spent[CityGood::Coin] > 0 && keep->escrow[CityGood::Coin] > 0, "the works resume and draw coin month by month");
    }

    // Cancelled works release their parcel.
    const auto keepId = c.project(CityProjectKind::Keep) ? c.project(CityProjectKind::Keep)->id : 0;
    const auto parcelsBefore = fabric.facts(100).freeParcels;

    c.realm.rejectProject(keepId, f.week, "the crown changed its mind", c.economy);
    ok = ok && c.advance();
    trace("cancellation");

    if (report)
    {
      check(ok && fabric.facts(100).freeParcels == parcelsBefore + 1 && c.site().constructions.count(keepId) == 0 && c.conserved(), "cancelled works release the parcel and refund the escrow");
    }

    // Roads: the busiest way is paved; closing a needed way is refused.
    c.crownProject(CityProjectKind::Road, 30, "road", "pave the busy lane");
    ok = ok && c.advance(16);
    trace("road");

    if (report)
    {
      check(ok && fabric.facts(100).streets >= 1 && c.last(CityFabricEventKind::RouteUpgraded) && c.last(CityFabricEventKind::RouteUpgraded)->detail.find("street") != std::string::npos, "a road project paves the busiest way into a street");
    }

    size_t refused = 0, closed = 0;

    for (const auto &[id, r] : std::map<uint64_t, CityRouteRecord>(c.site().routes))
    {
      std::string why;

      if (fabric.closeRoute(100, id, f.week, c.economy, f.homes, &why))
      {
        ++closed;

        break;
      }

      ++refused;
    }

    bool severed = false;

    for (const auto &[id, r] : c.site().routes)
    {
      std::string why;

      if (!r.closedWeek && !fabric.closeRoute(100, id, f.week, c.economy, f.homes, &why))
      {
        severed = true;

        break;
      }
    }

    ok = ok && c.advance();
    trace("closure");

    if (report)
    {
      check(ok && closed == 1 && refused >= 0 && c.count(CityFabricEventKind::RouteClosed) == 1, "an unneeded way can close and the city still validates");

      check(severed && c.count(CityFabricEventKind::RouteClosureRefused) >= 1, "closing a way that severs access is refused and recorded");

      uint32_t traffic = 0;

      for (const auto &[id, r] : c.site().routes)
      {
        traffic += r.traffic;
      }

      check(traffic > 0, "commuters leave traffic on the ways they walk");
    }

    return serialize(fabric);
  };

  const auto first = scenario(true);

  check(first == scenario(false), "identical runs produce identical fabric events");

  // --- Every layout and terrain plans and validates -------------------------------
  for (auto layout : {CityBaseLayout::Tapered, CityBaseLayout::Staggered, CityBaseLayout::Broad})
  {
    for (auto terrain : {CityBaseTerrain::Hillside, CityBaseTerrain::Ridge, CityBaseTerrain::Valley})
    {
      CityFabricLedger fabric;
      CityFabricSettings settings = hillside();

      settings.base.layout = layout;
      settings.base.terrain = terrain;
      settings.base.seed = 41;

      std::string error;
      const bool founded = fabric.found(100, settings, start, 4, &error);
      CityHouseholdPolicy housing;

      housing.housingCapacity[100] = fabric.facts(100).homes;

      Fixture f(housing);

      cast(f);

      City c(f, fabric);

      c.economy.grantStore(100, CityGood::Food, 200);
      check(founded && c.advance(2) && fabric.facts(100).homes == 4, "a layout and terrain pairing founds, houses and passes every spatial gate");
    }
  }

  return failures ? 1 : 0;
}
