// Phase 9 gates on a short matrix: no invariant failures, repeatable hashes,
// population and resource balances, bounded queues, and incremental versus
// full rebuild equivalence, across a stable village, a declining town and a
// growing city.
#include "procedural/world/WorldCityScale.hpp"
#include <iostream>
using namespace procedural::world::rewrite;

int main()
{
  int failures = 0;

  std::cout.setf(std::ios::unitbuf);

  auto check = [&](bool ok, const std::string &text) {
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
    failures += !ok;
  };

  {
    CityScaleSettings boundary;
    CityScaleReport result;
    boundary.years = CityScaleMaximumYears + 1;
    check(!runCityScale(boundary,result) && result.weeks==0, "unsupported history horizon rejected before simulation");
    boundary.years = 0;
    check(!runCityScale(boundary,result), "zero-year history rejected");
  }

  {
    CityScaleSettings rich;rich.enrichedHistory=true;rich.years=60;rich.seed=41;
    const auto p=cityScalePolicies(rich);
    check(p.economy.harvestPerMille.at(9)==550 && p.economy.harvestPerMille.at(11)==1200,
      "seeded poor harvests have a dated recovery policy");
    check(cityScaleSociety(rich).maximumFeudYears==4,"enriched feuds may persist across several years");
    for(auto job:{CityOccupation::Baker,CityOccupation::Brewer,CityOccupation::Weaver,CityOccupation::Potter,CityOccupation::Herbalist,CityOccupation::Scribe})
      check(p.economy.occupations[uint32_t(job)].wagePerMonth>0 && std::string(cityTradeOf(job)).size()>0,
        std::string(cityOccupationName(job))+" has a wage and a building trade");
  }
  {
    CitySocietySettings s;s.seed=41;s.settlements={{100,12}};s.feudsPerYear=20;s.maximumFeudYears=2;
    CitySocietyDriver driver(s);CityHistoryLedger ledger;std::string error;
    ledger.observe(driver.current(),&error);
    for(unsigned week=0;week<160;++week)ledger.observe(driver.step({}),&error);
    uint32_t ended=0;for(const auto &e:ledger.events())ended+=e.kind==CityHistoryEventKind::FeudEnded;
    check(ended>0,"64-bit dynasty pairs expire and emit feud endings instead of remaining permanent");
  }
  for (auto mode : {CityScaleMode::Village, CityScaleMode::Town, CityScaleMode::City})
  {
    const std::string name = cityScaleModeName(mode);
    CityScaleSettings settings;

    settings.mode = mode;
    settings.years = 25;
    settings.seed = 3;
    settings.maximumLiving = mode == CityScaleMode::City ? 600 : 300;

    CityScaleReport report, again;
    std::unique_ptr<CityChronicle> chronicle;
    const bool ok = runCityScale(settings, report, &chronicle);

    if (!ok)
    {
      std::cout << "  " << report.error << '\n';
    }

    check(ok, name + ": twenty-five years run with every weekly gate passing");
    check(runCityScale(settings, again) && again.hash == report.hash, name + ": the run is repeatable, hash for hash");
    check(report.coinConserved && report.foodBalanced, name + ": coin is conserved and food balances over the run");

    const auto &state = chronicle->state();
    uint32_t residents = 0, members = 0;

    for (const auto &[id, p] : state.previous->people)
    {
      residents += p.alive && p.settlement && state.fabric.sites().count(*p.settlement);
    }

    for (const auto &[hid, h] : state.homes.households())
    {
      if (!h.dissolvedWeek && h.settlement && state.fabric.sites().count(*h.settlement))
      {
        members += uint32_t(h.members.size());
      }
    }

    check(residents == members, name + ": every resident is in exactly one household");
    check(report.waitingHouseholds <= report.households && report.openProposals <= report.households * 4 + 16, name + ": waiting lists and proposals stay bounded by the households");

    std::cout << "  living " << report.startingLiving << " -> " << report.living << ", homes " << report.homes << ", buildings " << report.buildings << ", walls " << report.wallsStanding << ", stage " << report.stage << ", treasury " << report.treasury
              << ", hungry months " << report.hungryHouseholdMonths << ", " << report.totalSeconds << " s\n";

    if (mode == CityScaleMode::Village)
    {
      check(report.living >= report.startingLiving / 2 && report.living <= report.startingLiving * 3 && report.hungryHouseholdMonths == 0, "village: the population stays within a stable band and nobody goes hungry");
    }
    else if (mode == CityScaleMode::Town)
    {
      check(report.living < report.startingLiving, "town: the population declines");
    }
    else
    {
      check(report.living > report.startingLiving && report.homes > 30 && report.society.arrivals > 0, "city: the population, the homes and the newcomers grow");
    }

    // Incremental against full: the standing set at A plus the changes in
    // (A, B] is the standing set at B.
    for (const auto &[settlement, site] : state.fabric.sites())
    {
      const uint32_t a = site.foundedWeek + 200, b = site.foundedWeek + 25 * 52;
      std::map<uint64_t, uint64_t> standing;

      for (const auto &[id, v] : state.fabric.standingAt(settlement, a))
      {
        standing[id] = v->geometryHash;
      }

      for (const auto &change : state.fabric.changesBetween(settlement, a, b))
      {
        if (change.ended)
        {
          standing.erase(change.building);
        }

        if (change.began && (!change.version->toWeek || *change.version->toWeek > b))
        {
          standing[change.building] = change.version->geometryHash;
        }
      }

      std::map<uint64_t, uint64_t> full;

      for (const auto &[id, v] : state.fabric.standingAt(settlement, b))
      {
        full[id] = v->geometryHash;
      }

      check(standing == full && !full.empty(), name + ": rebuilding only what changed equals a full rebuild at the later date");
    }
  }

  return failures ? 1 : 0;
}
