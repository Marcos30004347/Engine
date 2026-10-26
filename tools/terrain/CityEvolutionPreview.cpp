#include "CityEvolutionMesh.hpp"
#include "CityHistoryJson.hpp"
#include "procedural/world/WorldCityHistoricalBrief.hpp"
#include "procedural/world/WorldCityScale.hpp"
#include <iostream>

int main(int argc, char **argv)
{
  try
  {
    if (argc < 2)
    {
      std::cerr << "Usage: CityEvolutionPreview OUTPUT [YEARS=1000] [SEED=41] [--hinterland] [--enriched-history] [--no-mesh] [--no-activities] [--activity-minute 0..1439]\n";
      return 2;
    }
    const std::filesystem::path root = argv[1];
    std::filesystem::create_directories(root);
    const auto save = [](const std::filesystem::path &path, const json &value) {
      std::ofstream file(path);
      file.exceptions(std::ios::failbit | std::ios::badbit);
      file << value.dump(2) << '\n';
      file.close();
    };
    CityScaleSettings settings;
    settings.mode = CityScaleMode::City;
    settings.keepStream = false;
    auto number = [](const char *value, uint64_t maximum) {
      const std::string text(value);
      size_t end = 0;
      if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
      {
        throw std::invalid_argument("expected an unsigned integer");
      }
      const auto result = std::stoull(text, &end);
      if (end != text.size() || result > maximum)
      {
        throw std::invalid_argument("numeric option exceeds supported range");
      }
      return uint32_t(result);
    };
    settings.years = 1000;
    settings.seed = 41;
    int option = 2;
    if (option < argc && std::string(argv[option]).rfind("--", 0) != 0)
      settings.years = number(argv[option++], CityScaleMaximumYears);
    if (option < argc && std::string(argv[option]).rfind("--", 0) != 0)
      settings.seed = number(argv[option++], UINT32_MAX);
    bool mesh = true, activities = true, hinterland=false;
    uint32_t activityMinute = 720;
    for (int i = option; i < argc; ++i)
    {
      const std::string flag = argv[i];
      if (flag == "--hinterland") {
        hinterland=true;settings.enrichedHistory=true;settings.settlements={{100,24,3,160,5},{200,6,1,72,2},{300,5,1,68,2},{400,3,0,60,2}};
        settings.routes={{100,200},{100,300},{200,400}};settings.seat=100;
      }
      else if (flag == "--enriched-history") {settings.enrichedHistory=true;}
      else if (flag == "--no-mesh")
      {
        mesh = false;
      }
      else if (flag == "--no-activities")
      {
        activities = false;
      }
      else if (flag == "--activity-minute" && i + 1 < argc)
      {
        const auto minute = number(argv[++i], 1439);
        if (minute >= 1440)
        {
          throw std::invalid_argument("activity minute must be 0..1439");
        }
        activityMinute = uint32_t(minute);
      }
      else
      {
        throw std::invalid_argument("unknown or incomplete option: " + flag);
      }
    }
    json milestones = json::array();
    settings.yearlyObserver = [&](uint32_t year, const CityState &state, std::string &error) {
      if (year % 10 == 0)
      {
        std::cerr << "year " << year << ", " << state.previous->people.size() << " people including ancestors\n";
      }
      if (year != 20 && year != 100 && year != 1000 && year != settings.years)
      {
        return true;
      }
      json summary = {{"year", year}, {"week", state.previous->week}, {"settlements", json::array()}};
      for (const auto &[settlement, site] : state.fabric.sites())
      {
        HistoricalCityBrief brief;
        if (!historicalCityBrief(state, settlement, brief, &error))
        {
          return false;
        }
        json j = historyJson(state, settlement, brief, site, "CitySocietyDriver scale fixture; not graph matcher", year);
        if(hinterland) {
          const std::map<uint32_t,std::pair<std::string,std::string>> names={{100,{"Alderhaven","city"}},{200,{"Willowford","village"}},{300,{"Stonebridge","village"}},{400,{"Barleyfields","farming hamlet"}}};
          j["name"]=names.at(settlement).first;j["role"]=names.at(settlement).second;
          j["regionalLinks"]=json::array();for(auto [a,b]:settings.routes)if(a==settlement || b==settlement)j["regionalLinks"].push_back(a==settlement?b:a);
        }
        j["harvestYears"]=json::array();
        for(auto [y,yield]:cityScalePolicies(settings).economy.harvestPerMille)if(y<=year)
          j["harvestYears"].push_back({{"year",y},{"yieldPerMille",yield}});
        auto optional = [](auto v) -> json {
          return v ? json(*v) : json(nullptr);
        };
        // The geometry first, so a person's day can be bound to the room it
        // happens in: a work activity in a building the history made a
        // smithy goes to its forge, sleep to a bedroom. A building the mesh
        // could not fit, or a room it does not have, stays null.
        const auto directory = root / ("year-" + std::to_string(year)) / ("settlement-" + std::to_string(settlement));
        std::filesystem::create_directories(directory);
        std::string meshError;
        bool rendered = false;
        CityModelPlan model;
        if (mesh && (hinterland ? year == settings.years : settlement == 100))
        {
          rendered = writeModel(site, state.fabric, state.defence, brief.week, directory / "city-model.json", meshError, &model);
        }
        std::map<uint64_t, const CityModelBuilding *> byParcel;
        for (const auto &b : model.buildings)
        {
          byParcel[b.id] = &b;
        }
        j["activityMinute"] = activities ? json(activityMinute) : json(nullptr);
        if (activities)
        {
          for (const auto &a : historicalDailyActivities(brief, activityMinute))
          {
            json room = nullptr;
            if (rendered && a.building)
            {
              const auto historical = site.buildings.find(*a.building);
              if (historical != site.buildings.end())
              {
                const auto physical = byParcel.find(historical->second.parcel);
                if (physical != byParcel.end())
                {
                  const auto bound = bindActivityRoom(*physical->second, a.activity);
                  if (bound)
                  {
                    room = *bound;
                  }
                }
              }
            }
            j["activities"].push_back({{"person", a.person}, {"activity", a.activity}, {"building", optional(a.building)}, {"room", room}});
          }
        }
        save(directory / "history.json", j);
        if(rendered) save(directory / "settlement.json", {{"historyProvenance", {
          {"snapshot", (directory/"history.json").string()}, {"settlement",settlement},
          {"week",brief.week},{"year",year}}}});
        summary["settlements"].push_back(
            {{"id", settlement},
             {"population", brief.demand.population},
             {"households", brief.demand.households},
             {"buildings", brief.buildings.size()},
             {"waitingHouseholds", brief.waitingHouseholds},
             {"defenceStage", j["defenceStage"]},
             {"mesh", rendered},
             {"meshError", meshError}});
      }
      milestones.push_back(summary);
      save(root / "milestones.json", milestones);
      return true;
    };
    CityScaleReport report;
    const bool ok = runCityScale(settings, report);
    json result = {
      {"ok", ok},
      {"seed", settings.seed},
      {"years", settings.years},
      {"error", report.error},
      {"seconds", report.totalSeconds},
      {"population", report.living},
      {"hash", report.hash},
      {"coinConserved", report.coinConserved},
      {"foodBalanced", report.foodBalanced},
      {"socialSource", "scale fixture"},
      {"milestones", milestones}};
    save(root / "report.json", result);
    std::cout << result.dump(2) << '\n';
    return ok ? 0 : 1;
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
