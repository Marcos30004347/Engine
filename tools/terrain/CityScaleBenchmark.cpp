// Phase 9: one cell of the scale matrix, timed and hashed. Run it twice with
// --repeat and the hashes must agree.
#include "json.hpp"
#include "procedural/world/WorldCityScale.hpp"
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;
using nlohmann::json;

int main(int argc, char **argv)
{
  if (argc < 4)
  {
    std::cerr << "Usage: CityScaleBenchmark village|town|city YEARS SEED [--no-stream] [--repeat] [--json OUT]\n";

    return 2;
  }

  CityScaleSettings settings;
  const std::string mode = argv[1];

  settings.mode = mode == "village" ? CityScaleMode::Village : mode == "town" ? CityScaleMode::Town : CityScaleMode::City;
  settings.years = uint32_t(std::stoul(argv[2]));
  settings.seed = uint32_t(std::stoul(argv[3]));

  bool repeat = false;
  std::string out;

  for (int i = 4; i < argc; ++i)
  {
    const std::string flag = argv[i];

    if (flag == "--no-stream")
    {
      settings.keepStream = false;
    }
    else if (flag == "--repeat")
    {
      repeat = true;
    }
    else if (flag == "--json" && i + 1 < argc)
    {
      out = argv[++i];
    }
  }

  auto describe = [&](const CityScaleReport &r) {
    json j = {{"mode", mode},
              {"years", settings.years},
              {"seed", settings.seed},
              {"weeks", r.weeks},
              {"living", r.living},
              {"livingEverywhere", r.livingEverywhere},
              {"startingLiving", r.startingLiving},
              {"households", r.households},
              {"homes", r.homes},
              {"buildings", r.buildings},
              {"wallsStanding", r.wallsStanding},
              {"stage", r.stage},
              {"freeParcels", r.freeParcels},
              {"events", r.events},
              {"eventBytes", r.eventBytes},
              {"streamBytes", r.streamBytes},
              {"ledgerSeconds", r.ledgerSeconds},
              {"totalSeconds", r.totalSeconds},
              {"slowestWeekSeconds", r.slowestWeekSeconds},
              {"msPerWeek", r.weeks ? r.totalSeconds * 1000 / r.weeks : 0},
              {"hash", r.hash},
              {"society", {{"births", r.society.births}, {"deaths", r.society.deaths}, {"marriages", r.society.marriages}, {"arrivals", r.society.arrivals}, {"departures", r.society.departures}, {"successions", r.society.successions}, {"feuds", r.society.feuds}}},
              {"treasury", r.treasury},
              {"storeCoin", r.storeCoin},
              {"householdCoin", r.householdCoin},
              {"unpaidWages", r.unpaidWages},
              {"employed", r.employed},
              {"unemployed", r.unemployed},
              {"foodStock", r.foodStock},
              {"hungryHouseholdMonths", r.hungryHouseholdMonths},
              {"openProposals", r.openProposals},
              {"waitingHouseholds", r.waitingHouseholds},
              {"coinConserved", r.coinConserved},
              {"foodBalanced", r.foodBalanced},
              {"blocked", r.blocked},
              {"crownProjects", r.crownProjects},
              {"settlements", r.settlements},
              {"storeTimber", r.storeTimber},
              {"storeStone", r.storeStone},
              {"error", r.error}};

    return j;
  };

  CityScaleReport first;
  const bool ok = runCityScale(settings, first);
  json report = describe(first);

  if (repeat && ok)
  {
    CityScaleReport second;

    runCityScale(settings, second);
    report["repeatable"] = second.hash == first.hash;
  }

  std::cout << report.dump(2) << '\n';

  if (!out.empty())
  {
    std::ofstream(out) << report.dump(2) << '\n';
  }

  return ok && (!repeat || report.value("repeatable", false)) ? 0 : 1;
}
