#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/StoryRunner.hpp"
#include "json.hpp"
#include "procedural/world/WorldCityChronicle.hpp"
#include "procedural/world/WorldCityDemand.hpp"
#include "procedural/world/WorldCityMigration.hpp"
#include "procedural/world/WorldCityModel.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace procedural::world::rewrite;
using nlohmann::json;

namespace {
std::string read(const std::string &path)
{
  std::ifstream in(path);

  if (!in)
  {
    throw std::runtime_error("cannot read " + path);
  }

  std::ostringstream out;

  out << in.rdbuf();

  return out.str();
}

json node(std::optional<HistoryNode> id)
{
  return id ? json(*id) : json(nullptr);
}

json weekOrNull(std::optional<uint32_t> week)
{
  return week ? json(*week) : json(nullptr);
}

json stockJson(const CityStock &s)
{
  json out = json::object();

  for (uint32_t g = 0; g < cityGoodCount; ++g)
  {
    out[cityGoodName(CityGood(g))] = s.units[g];
  }

  return out;
}

json poly(const std::vector<Position> &points)
{
  json out = json::array();

  for (const auto &p : points)
  {
    out.push_back({p.x, p.y, p.z});
  }

  return out;
}

unsigned long number(const char *text)
{
  const std::string value(text);
  size_t end = 0;

  if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
  {
    throw std::runtime_error("expected an unsigned integer");
  }

  const auto result = std::stoul(value, &end);

  if (end != value.size())
  {
    throw std::runtime_error("invalid integer");
  }

  return result;
}

// The preview's founding: a terraced site per settlement, a farm per authored
// house, one public workplace per trade, a granary and a purse. Authored
// placeholders that exercise every mechanism; the values are printed with
// the output and are not a balanced preset.
CityFoundingScript foundingScript(const CityHistorySnapshot &founding, std::vector<HistoryNode> &managed)
{
  CityFoundingScript script;
  std::map<HistoryNode, std::map<uint64_t, std::vector<HistoryNode>>> cohorts;
  std::map<HistoryNode, uint32_t> residents;

  for (const auto &[id, p] : founding.people)
  {
    if (p.alive && p.settlement)
    {
      cohorts[*p.settlement][p.dynasty ? uint64_t(*p.dynasty) << 1 : (uint64_t(id) << 1) | 1].push_back(id);
      ++residents[*p.settlement];
    }
  }

  uint32_t index = 0;

  for (const auto &[settlement, houses] : cohorts)
  {
    managed.push_back(settlement);

    CityFabricSettings site;

    site.base.seed = 41 + index * 30;
    site.base.tiers = 3;
    site.base.layout = index % 2 ? CityBaseLayout::Staggered : CityBaseLayout::Tapered;
    site.base.terrain = index % 2 ? CityBaseTerrain::Ridge : CityBaseTerrain::Hillside;
    script.sites[settlement] = site;
    ++index;

    for (const auto &[key, people] : houses)
    {
      if (key & 1)
      {
        continue; // A person of no house farms for nobody.
      }

      std::optional<HistoryNode> eldest;

      for (auto id : people)
      {
        const auto &p = founding.people.at(id);

        if ((p.stage == CityLifeStage::Adult || p.stage == CityLifeStage::Elder) && (!eldest || p.bornWeek < founding.people.at(*eldest).bornWeek))
        {
          eldest = id;
        }
      }

      if (!eldest)
      {
        continue;
      }

      CityWorkplaceFounding farm;

      farm.week = founding.week;
      farm.spec.settlement = settlement;
      farm.spec.occupation = CityOccupation::Farmer;
      farm.spec.slots = 3;
      farm.spec.owner = eldest;
      script.workplaces.push_back(farm);
    }

    const std::pair<CityOccupation, uint32_t> publicWork[] = {{CityOccupation::Labourer, 3}, {CityOccupation::Carpenter, 1}, {CityOccupation::Mason, 1},  {CityOccupation::Smith, 1},         {CityOccupation::Merchant, 1},
                                                              {CityOccupation::Innkeeper, 1}, {CityOccupation::Guard, 2},    {CityOccupation::Cleric, 1}, {CityOccupation::Administrator, 1}, {CityOccupation::Domestic, 1}};

    for (const auto &[occupation, slots] : publicWork)
    {
      CityWorkplaceFounding w;

      w.week = founding.week;
      w.spec.settlement = settlement;
      w.spec.occupation = occupation;
      w.spec.slots = slots;
      script.workplaces.push_back(w);
    }

    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Food, int64_t(residents[settlement]) * 4 * 3});
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Coin, 100});
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Timber, 60});
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Stone, 60});
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Tools, 5});
    script.grants.push_back({founding.week, settlement, 0, true, CityGood::Coin, 300});
  }

  script.foundingHouseholdCoin = 15;

  return script;
}

CityPolicies previewPolicies(const std::vector<HistoryNode> &managed)
{
  CityPolicies p;

  for (auto settlement : managed)
  {
    p.economy.tradeOpen.insert(settlement);
  }

  for (size_t i = 0; i + 1 < managed.size(); ++i)
  {
    p.economy.routes.insert({managed[i], managed[i + 1]});
  }

  return p;
}

// A plan view of one site at a date: parcels, what stands, the ways, the
// stairs, the walls. An orthographic comparison a reader can open anywhere.
std::string topView(const CityFabricSite &site, const CityDefenceLedger &defence, uint32_t week, const std::string &title)
{
  double minX = 1e300, minZ = 1e300, maxX = -1e300, maxZ = -1e300;

  for (const auto &deck : site.ground->relief.platforms)
  {
    for (const auto &p : deck.boundary)
    {
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
      minZ = std::min(minZ, p.z);
      maxZ = std::max(maxZ, p.z);
    }
  }

  minX -= 16;
  maxX += 16;
  minZ -= 24;
  maxZ += 16;

  const double scale = 6;
  const double w = (maxX - minX) * scale, h = (maxZ - minZ) * scale;
  std::ostringstream svg;
  auto X = [&](double x) { return (x - minX) * scale; };
  auto Y = [&](double z) { return h - (z - minZ) * scale; };
  auto ring = [&](const std::vector<Position> &pts, const char *fill, const char *stroke, double widthPx) {
    svg << "<polygon points=\"";

    for (const auto &p : pts)
    {
      svg << X(p.x) << ',' << Y(p.z) << ' ';
    }

    svg << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << widthPx << "\"/>\n";
  };

  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h + 30 << "\" viewBox=\"0 -30 " << w << ' ' << h + 30 << "\">\n";
  svg << "<rect x=\"0\" y=\"-30\" width=\"" << w << "\" height=\"" << h + 30 << "\" fill=\"#efe9dc\"/>\n";
  svg << "<text x=\"8\" y=\"-10\" font-family=\"sans-serif\" font-size=\"16\" fill=\"#3a332c\">" << title << "</text>\n";

  for (const auto &deck : site.ground->relief.platforms)
  {
    ring(deck.boundary, "#d8cfbd", "#9a8f7a", 1);
  }

  for (const auto &[id, parcel] : site.parcels)
  {
    ring(parcel.boundary, "#e6dfd0", "#b3a790", .6);
  }

  for (const auto &[id, r] : site.routes)
  {
    if (r.openedWeek > week || (r.closedWeek && *r.closedWeek <= week))
    {
      continue;
    }

    const char *colour = r.surface == CityRouteSurface::Street ? "#6b6560" : r.surface == CityRouteSurface::Lane ? "#9a927f" : "#c2b89f";

    svg << "<polyline fill=\"none\" stroke=\"" << colour << "\" stroke-width=\"" << r.halfWidth * 2 * scale << "\" stroke-linecap=\"round\" points=\"";

    for (const auto &p : r.path)
    {
      svg << X(p.x) << ',' << Y(p.z) << ' ';
    }

    svg << "\"/>\n";
  }

  for (const auto &[id, s] : site.structures)
  {
    if (s.kind == CityAccessKind::Deck || !s.completedWeek || *s.completedWeek > week)
    {
      continue;
    }

    svg << "<line x1=\"" << X(s.from.x) << "\" y1=\"" << Y(s.from.z) << "\" x2=\"" << X(s.to.x) << "\" y2=\"" << Y(s.to.z) << "\" stroke=\"#4d4640\" stroke-width=\"4\" stroke-dasharray=\"4,3\"/>\n";
  }

  for (const auto &[id, b] : site.buildings)
  {
    const auto *v = b.active(week);

    if (!v)
    {
      continue;
    }

    const char *fill = b.use == CityBuildingUse::Home ? "#b8543e" : b.use == CityBuildingUse::Keep ? "#3e3a5c" : b.use == CityBuildingUse::Manor ? "#5c4a7a" : b.use == CityBuildingUse::Church ? "#8a6a3a" : "#c98b3a";

    ring(v->footprint, fill, "#2d2823", .8);
  }

  CityCompletionPlan walls;

  defence.applyTo(site.settlement, week, walls);

  for (const auto &s : walls.wallSegments)
  {
    svg << "<line x1=\"" << X(s.a.x) << "\" y1=\"" << Y(s.a.z) << "\" x2=\"" << X(s.b.x) << "\" y2=\"" << Y(s.b.z) << "\" stroke=\"" << (s.lifecycle == CityWallLifecycle::Absorbed ? "#a09a90" : "#2b2622") << "\" stroke-width=\"" << std::max(2., s.width * scale) << "\"/>\n";
  }

  for (const auto &g : walls.gatehouses)
  {
    svg << "<circle cx=\"" << X(g.position.x) << "\" cy=\"" << Y(g.position.z) << "\" r=\"" << 2.4 * scale << "\" fill=\"#efe9dc\" stroke=\"#2b2622\" stroke-width=\"2\"/>\n";
  }

  for (const auto &t : walls.towers)
  {
    svg << "<rect x=\"" << X(t.position.x) - t.radius * scale << "\" y=\"" << Y(t.position.z) - t.radius * scale << "\" width=\"" << t.radius * 2 * scale << "\" height=\"" << t.radius * 2 * scale << "\" fill=\"#2b2622\"/>\n";
  }

  svg << "</svg>\n";

  return svg.str();
}

// The mesh the existing render script reads, built from the plan at a date.
bool writeModel(const CityFabricSite &site, const CityFabricLedger &fabric, const CityDefenceLedger &defence, uint32_t week, const std::filesystem::path &path, std::string &error)
{
  CityCompletionPlan plan = fabric.completionAt(site.settlement, week);

  defence.applyTo(site.settlement, week, plan);

  SettlementFabric ground;

  ground.terrain = site.ground->relief.openGround;

  CityModelSettings options;

  options.groundVertexSpacing = 4;

  // A stair not yet built at this date is not meshed: the relief plan carries
  // every flight the base reserved, the ledger says which ones stand.
  CityReliefPlan relief = site.ground->relief;
  std::vector<Position> unbuilt; // Every point of every access path whose stair is not yet finished.

  for (const auto &[id, s] : site.structures)
  {
    if ((s.kind != CityAccessKind::Stair && s.kind != CityAccessKind::Approach) || (s.completedWeek && *s.completedWeek <= week))
    {
      continue;
    }

    for (const auto &deck : site.ground->relief.platforms)
    {
      if (deck.block == s.block)
      {
        unbuilt.push_back(deck.accessFrom);
        unbuilt.push_back(deck.accessTo);
        unbuilt.insert(unbuilt.end(), deck.accessPath.begin(), deck.accessPath.end());
      }
    }
  }

  auto near = [&](const Position &p) {
    for (const auto &q : unbuilt)
    {
      if (std::hypot(p.x - q.x, p.z - q.z) < 3)
      {
        return true;
      }
    }

    return false;
  };

  relief.stairs.erase(std::remove_if(relief.stairs.begin(), relief.stairs.end(), [&](const CityReliefPlan::Stairway &stair) { return near(stair.a) || near(stair.b); }), relief.stairs.end());
  relief.stairLandings.erase(std::remove_if(relief.stairLandings.begin(), relief.stairLandings.end(),
                                            [&](const CityReliefPlan::StairLanding &landing) {
                                              for (const auto &p : landing.boundary)
                                              {
                                                if (near(p))
                                                {
                                                  return true;
                                                }
                                              }

                                              return false;
                                            }),
                             relief.stairLandings.end());

  CityModelPlan model;

  if (!buildCityModel(plan, ground, options, model, &error, &relief))
  {
    return false;
  }

  json j;

  j["sourcePipeline"] = "native-district";
  j["generation"] = {{"mode", "living-city-history"}, {"week", week}, {"settlement", site.settlement}};
  j["vertices"] = json::array();

  for (auto p : model.mesh.vertices)
  {
    j["vertices"].push_back({p.x, p.y, p.z});
  }

  j["triangles"] = model.mesh.triangles;
  j["materials"] = model.materials;
  j["triangleUVs"] = model.triangleUVs;
  j["surfaceMaterials"] = model.surfaceMaterials;
  j["groundWeights"] = model.groundWeights;
  j["groundMaterialIds"] = model.groundMaterialIds;
  j["textureManifest"] = "assets/procedural/textures/medieval-pixel/materials.json";
  j["buildings"] = json::array();

  for (const auto &b : model.buildings)
  {
    j["buildings"].push_back({{"id", b.id},
                              {"source", b.source},
                              {"kind", cityModelBuildingKindName(b.kind)},
                              {"footprint", poly(b.footprint)},
                              {"base", b.base},
                              {"wallHeight", b.wallHeight},
                              {"roofHeight", b.roofHeight},
                              {"rooms", b.rooms},
                              {"windows", b.windows},
                              {"roomConnections", b.roomConnections},
                              {"exteriorDoors", poly(b.exteriorDoors)},
                              {"jettyMetres", b.jettyMetres},
                              {"interiorViews", poly(b.interiorViews)}});
  }

  j["rejections"] = model.rejections;
  j["wallSegments"] = plan.wallSegments.size();
  std::ofstream(path) << j.dump();

  return true;
}
} // namespace

int main(int argc, char **argv)
{
  try
  {
    bool verifyReplay = false, migrate = true;
    std::optional<std::filesystem::path> renders;

    for (int i = 5; i < argc; ++i)
    {
      const std::string flag = argv[i];

      if (flag == "--verify-replay")
      {
        verifyReplay = true;
      }
      else if (flag == "--no-migration")
      {
        migrate = false;
      }
      else if (flag == "--renders" && i + 1 < argc)
      {
        renders = argv[++i];
      }
      else
      {
        argc = 0;
      }
    }

    if (argc < 5)
    {
      std::cerr << "Usage: CityHistoryPreview ASSET_DIRECTORY OUTPUT.json YEARS SEED [--verify-replay] [--no-migration] [--renders DIR]\n";

      return 2;
    }

    const auto years = number(argv[3]), seed = number(argv[4]);

    if (years < 1 || years > 200 || seed > UINT32_MAX)
    {
      throw std::runtime_error("years must be 1..200 and seed must fit uint32");
    }

    const std::string root = argv[1];
    DiagnosticSink diagnostics;
    compiler::CompiledProgram program;
    compiler::UsageValues usage;
    LoadedFacts facts;
    Narrator narrator;
    StorySettings settings;

    settings.seed = uint32_t(seed);
    settings.ticks = uint32_t(years * 52);
    settings.populationCap = 0; // The story's cap is a death multiplier; the city guards population by suspending arrivals instead.
    settings.recordEvents = false;
    facts.valueResolver = [&](uint32_t year, uint32_t &id) {
      if (uint64_t(year) * 52 + settings.dayBase > UINT32_MAX)
      {
        return false;
      }

      id = settings.dayBase + year * 52;

      return true;
    };

    if (!compiler::compile(read(root + "/story.graphwrite"), "story.graphwrite", {}, diagnostics, program, usage) ||
        !loadFactsText(read(root + "/story.graphfacts"), program.edgeTypes, "story.graphfacts", diagnostics, facts))
    {
      for (const auto &d : diagnostics.all())
      {
        std::cerr << d.format("history") << '\n';
      }

      return 1;
    }

    std::cerr << "Story rules compiled; simulating " << years << " years\n";

    auto simulate = [&]() {
      StoryRunner runner(program, usage, facts, narrator, settings);
      std::optional<CityChronicle> chronicle;
      std::optional<CityMigrationAdapter> migration;
      std::vector<HistoryNode> managed;
      json samples = json::array(), policy;
      std::string error;

      for (uint32_t tick = 0; tick <= settings.ticks; ++tick)
      {
        if (tick)
        {
          runner.step(); // Do not stop on quiet weeks: age and schedules still advance.
        }

        if (runner.overflowedWeeks())
        {
          throw std::runtime_error("story matcher overflow: history is incomplete");
        }

        const uint32_t week = settings.startWeek + tick;
        CityHistorySnapshot snapshot;

        if (!readCityHistorySnapshot(program.edgeTypes, runner.graph(), week, settings.dayBase, snapshot, &error))
        {
          throw std::runtime_error("week " + std::to_string(tick) + ": " + error);
        }

        if (!chronicle)
        {
          const auto script = foundingScript(snapshot, managed);
          const auto policies = previewPolicies(managed);

          chronicle.emplace(policies, script);
          policy = {{"policies", cityPoliciesJson(policies)}, {"script", cityScriptJson(script)}};

          if (migrate)
          {
            CityMigrationPolicy rules;

            rules.origins = {"Eoferwic", "Lindcylne"};

            for (auto settlement : managed)
            {
              rules.destinations[settlement] = {0.6, 0.6, 0.6};
            }

            rules.reviewIntervalWeeks = 52;
            rules.householdsPerReview = 2;
            rules.waitingTolerance = 1;
            rules.patienceWeeks = 52;
            rules.maxLivingPeople = 400;
            rules.seed = uint32_t(seed);
            migration.emplace(rules, program.edgeTypes, runner.mutableGraph(), runner.names(), settings.dayBase);

            if (!migration->ready(&error))
            {
              throw std::runtime_error(error);
            }

            policy["migration"] = {{"origins", rules.origins}, {"reviewIntervalWeeks", rules.reviewIntervalWeeks}, {"householdsPerReview", rules.householdsPerReview}, {"waitingTolerance", rules.waitingTolerance}, {"patienceWeeks", rules.patienceWeeks}, {"maxLivingPeople", rules.maxLivingPeople}};
          }
        }

        if (!chronicle->observe(snapshot, &error))
        {
          throw std::runtime_error("week " + std::to_string(tick) + ": " + error);
        }

        const auto &state = chronicle->state();

        if (migration)
        {
          for (auto settlement : managed)
          {
            const auto economy = state.economy.facts(settlement, state.homes);
            CitySettlementProspects prospects;

            prospects.work = economy.work;
            prospects.food = economy.food;
            prospects.safety = state.realm.safety(settlement, snapshot, state.economy, state.homes);
            migration->setProspects(settlement, prospects);
          }
        }

        if (tick % 52 == 0)
        {
          json people = json::array(), settlements = json::array(), rulers = json::array(), households = json::array(), workplaces = json::array(), realms = json::array(), projects = json::array(), sites = json::array();
          std::set<HistoryNode> places;
          auto livelihood = [&](HistoryNode id) -> json {
            const auto it = state.economy.livelihoods().find(id);

            if (it == state.economy.livelihoods().end())
            {
              return nullptr;
            }

            const auto &r = it->second;

            return {{"state", r.adult ? cityLivelihoodName(r.livelihood) : cityDependencyName(r.dependency)},
                    {"socialClass", citySocialClassName(r.socialClass)},
                    {"occupation", r.occupation ? json(cityOccupationName(*r.occupation)) : json(nullptr)},
                    {"workplace", r.workplace ? json(*r.workplace) : json(nullptr)},
                    {"mentor", node(r.mentor)}};
          };

          for (const auto &[id, p] : snapshot.people)
          {
            people.push_back({{"id", id},
                              {"name", runner.names().nameOf(id)},
                              {"bornWeek", p.bornWeek},
                              {"alive", p.alive},
                              {"settlement", node(p.settlement)},
                              {"dynasty", node(p.dynasty)},
                              {"spouse", node(p.spouse)},
                              {"parents", p.parents},
                              {"ancestryKnown", !p.parents.empty()},
                              {"household", state.homes.householdOf(id) ? json(*state.homes.householdOf(id)) : json(nullptr)},
                              {"lifeStage", int(p.stage)},
                              {"socialRole", int(p.role)},
                              {"livelihood", livelihood(id)}});

            if (p.settlement)
            {
              places.insert(*p.settlement);
            }
          }

          for (const auto &[id, h] : state.homes.households())
          {
            if (h.dissolvedWeek)
            {
              continue;
            }

            households.push_back({{"id", id},
                                  {"settlement", node(h.settlement)},
                                  {"origin", node(h.origin)},
                                  {"kind", cityHouseholdOriginName(h.kind)},
                                  {"foundedWeek", h.foundedWeek},
                                  {"head", node(h.head)},
                                  {"members", std::vector<HistoryNode>(h.members.begin(), h.members.end())},
                                  {"housing", cityHousingStateName(h.housing)},
                                  {"waitingSince", weekOrNull(h.waitingSince)},
                                  {"unguarded", h.unguarded},
                                  {"dependentElders", h.dependentElders}});
          }

          for (auto place : places)
          {
            auto demand = cityHistoryDemand(snapshot, place);
            CityPopulationDemand land;

            if (!deriveCityPopulationDemand(cityHistoryHousingEstimates(demand), {}, land, &error))
            {
              throw std::runtime_error(error);
            }

            const auto capacity = state.homes.policy().housingCapacity.find(place);
            const auto economy = state.economy.facts(place, state.homes);
            const auto town = state.economy.settlements().find(place);
            const auto fabric = state.fabric.facts(place);
            int64_t hungry = 0;

            for (const auto &[hid, h] : state.homes.households())
            {
              if (!h.dissolvedWeek && h.settlement == place && state.economy.accounts().count(hid))
              {
                hungry += state.economy.accounts().at(hid).hungryMonths;
              }
            }

            settlements.push_back({{"id", place},
                                   {"name", runner.names().nameOf(place)},
                                   {"origin", migration && migration->isOrigin(place)},
                                   {"residents", demand.residents},
                                   {"children", demand.children},
                                   {"apprenticeAge", demand.apprenticeAge},
                                   {"workingAge", demand.workingAge},
                                   {"elders", demand.elders},
                                   {"households", state.homes.livingHouseholds(place)},
                                   {"housedHouseholds", state.homes.housedHouseholds(place)},
                                   {"waitingHouseholds", state.homes.waitingList(place).size()},
                                   {"housingCapacity", capacity == state.homes.policy().housingCapacity.end() ? json(nullptr) : json(capacity->second)},
                                   {"provisionalFamilyCohorts", demand.familyCohorts.size()},
                                   {"provisionalProgrammeLandArea", land.programmeLandArea},
                                   {"provisionalCirculationArea", land.circulationArea},
                                   {"economy",
                                    {{"workingAge", economy.workingAge},
                                     {"employed", economy.employed},
                                     {"unemployed", economy.unemployed},
                                     {"apprentices", economy.apprentices},
                                     {"freeSlots", economy.freeSlots},
                                     {"guards", economy.guards},
                                     {"merchants", economy.merchants},
                                     {"foodStock", economy.foodStock},
                                     {"foodNeedPerMonth", economy.foodNeedPerMonth},
                                     {"storeCoin", economy.storeCoin},
                                     {"treasury", economy.treasury},
                                     {"householdCoin", economy.householdCoin},
                                     {"hungryHouseholdMonths", hungry},
                                     {"store", town == state.economy.settlements().end() ? json(nullptr) : stockJson(town->second.store)}}},
                                   {"fabric",
                                    {{"homes", fabric.homes},
                                     {"occupiedHomes", fabric.occupiedHomes},
                                     {"vacantHomes", fabric.vacantHomes},
                                     {"freeParcels", fabric.freeParcels},
                                     {"reachableFreeParcels", fabric.reachableFreeParcels},
                                     {"underConstruction", fabric.underConstruction},
                                     {"occupiedTerraces", fabric.occupiedBlocks.size()},
                                     {"streets", fabric.streets},
                                     {"lanes", fabric.lanes},
                                     {"footpaths", fabric.footpaths}}},
                                   {"prospects", {{"work", economy.work}, {"food", economy.food}, {"safety", state.realm.safety(place, snapshot, state.economy, state.homes)}}}});
          }

          for (const auto &[id, w] : state.economy.workplaces())
          {
            json apprentices = json::array();

            for (const auto &[a, m] : w.apprentices)
            {
              apprentices.push_back({{"apprentice", a}, {"mentor", m}});
            }

            workplaces.push_back({{"id", id}, {"settlement", w.settlement}, {"occupation", cityOccupationName(w.occupation)}, {"slots", w.slots}, {"owner", node(w.owner)}, {"retired", w.retiredWeek.has_value()}, {"workers", w.workers}, {"apprentices", apprentices}});
          }

          for (const auto &[id, c] : state.realm.realms())
          {
            realms.push_back({{"realm", id},
                              {"name", runner.names().nameOf(id)},
                              {"ruler", node(c.ruler)},
                              {"consort", node(c.consort)},
                              {"heir", node(c.heir)},
                              {"regent", node(c.regent)},
                              {"claimants", std::vector<HistoryNode>(c.claimants.begin(), c.claimants.end())},
                              {"seat", node(c.seat)},
                              {"vacant", c.vacant},
                              {"authority", cityAuthorityName(c.authority)},
                              {"authorityReason", c.authorityReason}});
          }

          std::vector<std::pair<uint64_t, const CityProject *>> every;

          for (const auto &[id, p] : state.realm.projects())
          {
            every.push_back({id, &p});
          }

          for (const auto &[id, p] : state.realm.closedProjects())
          {
            every.push_back({id, &p});
          }

          for (const auto &[id, pp] : every)
          {
            const auto &p = *pp;

            projects.push_back({{"id", id},
                                {"kind", cityProjectKindName(p.intent.kind)},
                                {"settlement", p.intent.settlement},
                                {"sponsor", p.intent.sponsor == CitySponsorKind::Treasury ? "treasury" : "household"},
                                {"household", p.intent.household},
                                {"sponsorPerson", node(p.intent.sponsorPerson)},
                                {"state", cityProjectStateName(p.state)},
                                {"priority", p.intent.priority},
                                {"reason", p.intent.reason},
                                {"blockedBy", p.blockedBy},
                                {"site", p.intent.site ? json(*p.intent.site) : json(nullptr)},
                                {"cost", stockJson(p.cost)},
                                {"escrow", stockJson(p.escrow)},
                                {"spent", stockJson(p.spent)},
                                {"proposedWeek", p.proposedWeek},
                                {"fundedWeek", weekOrNull(p.fundedWeek)},
                                {"startedWeek", weekOrNull(p.startedWeek)},
                                {"completedWeek", weekOrNull(p.completedWeek)}});
          }

          for (const auto &[settlement, site] : state.fabric.sites())
          {
            json buildings = json::array(), routes = json::array(), structures = json::array(), circuits = json::array();

            for (const auto &[id, b] : site.buildings)
            {
              const auto *v = b.standing();

              buildings.push_back({{"id", id},
                                   {"parcel", b.parcel},
                                   {"terrace", site.parcels.at(b.parcel).block},
                                   {"use", cityBuildingUseName(b.use)},
                                   {"versions", b.versions.size()},
                                   {"standing", v != nullptr},
                                   {"floors", v ? json(v->floors) : json(nullptr)},
                                   {"since", v ? json(v->fromWeek) : json(nullptr)},
                                   {"hash", v ? json(v->geometryHash) : json(nullptr)},
                                   {"tenant", b.tenant ? json(*b.tenant) : json(nullptr)},
                                   {"workplace", b.workplace ? json(*b.workplace) : json(nullptr)},
                                   {"sponsorPerson", node(b.sponsorPerson)}});
            }

            for (const auto &[id, r] : site.routes)
            {
              routes.push_back({{"id", id}, {"surface", cityRouteSurfaceName(r.surface)}, {"open", !r.closedWeek}, {"traffic", r.traffic}});
            }

            for (const auto &[id, s] : site.structures)
            {
              structures.push_back({{"id", id}, {"kind", cityAccessKindName(s.kind)}, {"terrace", s.block}, {"state", cityStructureStateName(s.state)}, {"completedWeek", weekOrNull(s.completedWeek)}, {"treads", s.treads}, {"landings", s.landings}});
            }

            const auto defence = state.defence.sites().find(settlement);

            if (defence != state.defence.sites().end())
            {
              for (const auto &c : defence->second.circuits)
              {
                uint32_t built = 0;

                for (const auto &s : c.segments)
                {
                  built += s.state == CityStructureState::Completed;
                }

                circuits.push_back({{"id", c.id},
                                    {"era", c.era},
                                    {"palisade", c.palisade},
                                    {"lifecycle", cityWallLifecycleName(c.lifecycle)},
                                    {"closed", c.closed()},
                                    {"abandoned", c.abandoned},
                                    {"segmentsBuilt", built},
                                    {"segments", c.segments.size()},
                                    {"gates", c.gates.size()},
                                    {"towers", c.towers.size()},
                                    {"enclosedTerraces", std::vector<uint64_t>(c.enclosedBlocks.begin(), c.enclosedBlocks.end())},
                                    {"closedWeek", weekOrNull(c.closedWeek)},
                                    {"absorbedWeek", weekOrNull(c.absorbedWeek)}});
              }
            }

            sites.push_back({{"settlement", settlement},
                             {"foundedWeek", site.foundedWeek},
                             {"terraces", site.ground->relief.platforms.size()},
                             {"parcels", site.parcels.size()},
                             {"buildings", buildings},
                             {"routes", routes},
                             {"structures", structures},
                             {"constructions", site.constructions.size()},
                             {"defenceStage", defence == state.defence.sites().end() ? "none" : cityDefenceStageName(defence->second.stage)},
                             {"circuits", circuits}});
          }

          for (const auto &[realm, ruler] : snapshot.rulers)
          {
            rulers.push_back({{"realm", realm}, {"ruler", ruler}});
          }

          samples.push_back({{"week", snapshot.week}, {"people", people}, {"households", households}, {"settlements", settlements}, {"rulers", rulers}, {"workplaces", workplaces}, {"realms", realms}, {"projects", projects}, {"sites", sites}});

          if (renders)
          {
            const uint32_t year = tick / 52;

            if (year == 0 || year == 3 || year == 6 || year == years)
            {
              std::filesystem::create_directories(*renders);

              for (const auto &[settlement, site] : state.fabric.sites())
              {
                const std::string name = runner.names().nameOf(settlement) + "-year" + std::to_string(year);

                std::ofstream(*renders / (name + ".svg")) << topView(site, state.defence, week, runner.names().nameOf(settlement) + ", year " + std::to_string(year));

                if (!writeModel(site, state.fabric, state.defence, week, *renders / (name + ".json"), error))
                {
                  std::cerr << "render " << name << ": " << error << '\n';
                }
              }
            }
          }
        }

        // Publish next week's migration facts from this week's validated ledgers.
        if (migration && !migration->publish(week, snapshot, state.homes, &error))
        {
          throw std::runtime_error("week " + std::to_string(tick) + ": " + error);
        }
      }

      const auto &state = chronicle->state();
      json events = json::array(), householdEvents = json::array(), migrationEvents = json::array(), economyEvents = json::array(), realmEvents = json::array(), fabricEvents = json::array(), defenceEvents = json::array(), origins = json::object();

      for (const auto &e : state.social.events())
      {
        events.push_back({{"week", e.week}, {"kind", cityHistoryEventName(e.kind)}, {"subject", e.subject}, {"before", node(e.before)}, {"after", node(e.after)}});
      }

      for (const auto &e : state.homes.events())
      {
        householdEvents.push_back({{"week", e.week}, {"kind", cityHouseholdEventName(e.kind)}, {"household", e.household}, {"person", node(e.person)}, {"before", node(e.before)}, {"after", node(e.after)}, {"cause", e.cause}});
      }

      for (const auto &e : state.economy.events())
      {
        economyEvents.push_back({{"week", e.week}, {"kind", cityEconomyEventName(e.kind)}, {"person", node(e.person)}, {"settlement", node(e.settlement)}, {"household", e.household}, {"workplace", e.workplace}, {"good", cityGoodName(e.good)}, {"amount", e.amount}});
      }

      for (const auto &e : state.realm.events())
      {
        realmEvents.push_back({{"week", e.week}, {"kind", cityRealmEventName(e.kind)}, {"realm", node(e.realm)}, {"settlement", node(e.settlement)}, {"person", node(e.person)}, {"before", node(e.before)}, {"after", node(e.after)}, {"asset", e.asset}, {"amount", e.amount}, {"reason", e.reason}});
      }

      for (const auto &e : state.fabric.events())
      {
        fabricEvents.push_back({{"week", e.week}, {"kind", cityFabricEventName(e.kind)}, {"settlement", e.settlement}, {"subject", e.subject}, {"household", e.household}, {"detail", e.detail}});
      }

      for (const auto &e : state.defence.events())
      {
        defenceEvents.push_back({{"week", e.week}, {"kind", cityDefenceEventName(e.kind)}, {"settlement", e.settlement}, {"subject", e.subject}, {"detail", e.detail}});
      }

      if (migration)
      {
        for (const auto &e : migration->events())
        {
          migrationEvents.push_back({{"week", e.week}, {"kind", cityMigrationEventName(e.kind)}, {"household", e.household}, {"origin", node(e.origin)}, {"destination", node(e.destination)}, {"people", e.people}});
        }

        for (const auto &[id, name] : migration->origins())
        {
          origins[std::to_string(id)] = name;
        }
      }

      // Phase 8 on the real story: the recorded stream replays to the same
      // hash, and a saved chronicle reloads to it.
      const auto liveHash = chronicle->stateHash();
      CityState replayed;

      if (!chronicle->replay({}, replayed, &error))
      {
        throw std::runtime_error("replay failed: " + error);
      }

      const auto replayHash = CityChronicle::hashOf(replayed);
      CityChronicle reloaded(chronicle->policies(), chronicle->script());

      if (!CityChronicle::load(chronicle->save(), reloaded, &error))
      {
        throw std::runtime_error("reload failed: " + error);
      }

      const auto reloadHash = reloaded.stateHash();

      if (replayHash != liveHash || reloadHash != liveHash)
      {
        throw std::runtime_error("checkpoint replay or reload does not match the live state");
      }

      json result = {
        {"schemaVersion", 4},
        {"seed", seed},
        {"startWeek", settings.startWeek},
        {"years", years},
        {"lifeStages", {"child", "apprentice-age", "adult", "elder"}},
        {"socialRoles", {"resident", "clergy", "heir", "consort", "ruler"}},
        {"scope", "Graph-driven social history; households and migration; livelihoods and budgets; offices, inheritance and projects; terraced sites with persistent parcels, building versions, construction, routes and stairs; manor to castle with walls in funded segments; checkpoint replay and inspection."},
        {"policy", policy},
        {"origins", origins},
        {"checkpoint", {{"weeks", chronicle->stream().size()}, {"hash", liveHash}, {"replayMatches", replayHash == liveHash}, {"reloadMatches", reloadHash == liveHash}}},
        {"series", chronicle->series()},
        {"snapshots", samples},
        {"events", events},
        {"householdEvents", householdEvents},
        {"migrationEvents", migrationEvents},
        {"economyEvents", economyEvents},
        {"realmEvents", realmEvents},
        {"fabricEvents", fabricEvents},
        {"defenceEvents", defenceEvents}};

      return result;
    };

    const json result = simulate();

    if (verifyReplay)
    {
      if (result.dump() != simulate().dump())
      {
        throw std::runtime_error("deterministic replay differs");
      }

      std::cerr << "Repeated simulation is byte-for-byte deterministic\n";
    }

    std::ofstream file(argv[2]);

    file << result.dump(2) << '\n';

    if (!file)
    {
      throw std::runtime_error("cannot write output");
    }

    std::cout << years << " years, " << result.at("snapshots").back().at("people").size() << " people including ancestors, " << result.at("events").size() << " history events, "
              << result.at("householdEvents").size() << " household events, " << result.at("migrationEvents").size() << " migration events, " << result.at("economyEvents").size() << " economy events, "
              << result.at("realmEvents").size() << " realm events, " << result.at("fabricEvents").size() << " fabric events, " << result.at("defenceEvents").size() << " defence events; checkpoint hash " << result.at("checkpoint").at("hash") << '\n';

    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';

    return 1;
  }
}
