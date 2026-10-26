#include "WorldCityChronicle.hpp"
#include <chrono>
#include <sstream>

namespace procedural::world::rewrite {
namespace {
using nlohmann::json;

bool fail(std::string *error, const std::string &message)
{
  if (error)
  {
    *error = message;
  }

  return false;
}

json node(std::optional<HistoryNode> id)
{
  return id ? json(*id) : json(nullptr);
}

std::optional<HistoryNode> nodeOf(const json &j)
{
  return j.is_null() ? std::nullopt : std::optional<HistoryNode>(j.get<HistoryNode>());
}

json stockJson(const CityStock &s)
{
  return json(s.units);
}

CityStock stockOf(const json &j)
{
  CityStock s;

  for (uint32_t g = 0; g < cityGoodCount && g < j.size(); ++g)
  {
    s.units[g] = j[g].get<int64_t>();
  }

  return s;
}

json positionJson(const Position &p)
{
  return {p.x, p.y, p.z};
}

uint64_t fnv(uint64_t h, const std::string &text)
{
  for (unsigned char c : text)
  {
    h ^= c;
    h *= 1099511628211ull;
  }

  return h;
}

uint32_t cohortsAt(const CityHistorySnapshot &snapshot, HistoryNode settlement)
{
  std::set<uint64_t> cohorts;

  for (const auto &[id, p] : snapshot.people)
  {
    if (p.alive && p.settlement == settlement)
    {
      cohorts.insert(p.dynasty ? uint64_t(*p.dynasty) << 1 : (uint64_t(id) << 1) | 1);
    }
  }

  return uint32_t(cohorts.size());
}
} // namespace

// --- policies and script -------------------------------------------------------

json cityPoliciesJson(const CityPolicies &p)
{
  json households = json::object();

  for (const auto &[settlement, capacity] : p.households.housingCapacity)
  {
    households[std::to_string(settlement)] = capacity;
  }

  json occupations = json::array();

  for (const auto &o : p.economy.occupations)
  {
    occupations.push_back({{"skillRequired", o.skillRequired},
                           {"wagePerMonth", o.wagePerMonth},
                           {"inputsPerWorker", stockJson(o.inputsPerWorker)},
                           {"outputsPerWorker", stockJson(o.outputsPerWorker)},
                           {"publicService", o.publicService},
                           {"needsLand", o.needsLand},
                           {"needsTrade", o.needsTrade},
                           {"clergyOnly", o.clergyOnly}});
  }

  json routes = json::array(), tradeOpen = json::array(), harvest = json::object(), costs = json::array();

  for (const auto &[a, b] : p.economy.routes)
  {
    routes.push_back({a, b});
  }

  for (auto s : p.economy.tradeOpen)
  {
    tradeOpen.push_back(s);
  }

  for (const auto &[year, permille] : p.economy.harvestPerMille)
  {
    harvest[std::to_string(year)] = permille;
  }

  for (const auto &c : p.projects.costs)
  {
    costs.push_back(stockJson(c));
  }

  const auto &e = p.economy;
  const auto &d = p.defence;

  return {{"households", {{"housingCapacity", households}, {"independenceAgeYears", p.households.independenceAgeYears ? json(*p.households.independenceAgeYears) : json(nullptr)}}},
          {"economy",
           {{"occupations", occupations},
            {"prices", e.prices},
            {"foodPerPersonPerMonth", e.foodPerPersonPerMonth},
            {"subsistenceFoodPerPersonPerMonth", e.subsistenceFoodPerPersonPerMonth},
            {"upkeepPerHouseholdPerMonth", e.upkeepPerHouseholdPerMonth},
            {"upkeepPerPersonPerMonth", e.upkeepPerPersonPerMonth},
            {"householdSpendingPerMille", e.householdSpendingPerMille},
            {"wealthTaxPerMille", e.wealthTaxPerMille},
            {"treasurySeat", [&] {
               json seats = json::array();

               for (const auto &[a, b] : e.treasurySeat)
               {
                 seats.push_back({a, b});
               }

               return seats;
             }()},
            {"householdCoinReserve", e.householdCoinReserve},
            {"marketDuesPerMille", e.marketDuesPerMille},
            {"storeFloatCoin", e.storeFloatCoin},
            {"taxPerMille", e.taxPerMille},
            {"monthWeeks", e.monthWeeks},
            {"reserveMonths", e.reserveMonths},
            {"householdGoodsReserve", e.householdGoodsReserve},
            {"exportReserve", stockJson(e.exportReserve)},
            {"importFloor", stockJson(e.importFloor)},
            {"apprenticesPerMentor", e.apprenticesPerMentor},
            {"mentorSkill", e.mentorSkill},
            {"maxSkill", e.maxSkill},
            {"retirementAge", e.retirementAge},
            {"tradeUnitsPerMerchant", e.tradeUnitsPerMerchant},
            {"routes", routes},
            {"tradeOpen", tradeOpen},
            {"harvestPerMille", harvest},
            {"seed", e.seed}}},
          {"succession", {{"majorityAge", p.succession.majorityAge}, {"regencyByConsort", p.succession.regencyByConsort}, {"regencyByKin", p.succession.regencyByKin}, {"electionFallback", p.succession.electionFallback}}},
          {"projects",
           {{"costs", costs},
            {"reviewWeeks", p.projects.reviewWeeks},
            {"marketHouseholds", p.projects.marketHouseholds},
            {"keepHouseholds", p.projects.keepHouseholds},
            {"wallHouseholds", p.projects.wallHouseholds},
            {"keepSafety", p.projects.keepSafety},
            {"wallSafety", p.projects.wallSafety}}},
          {"defence",
           {{"segmentMetres", d.segmentMetres},
            {"towerSpacingMetres", d.towerSpacingMetres},
            {"marginMetres", d.marginMetres},
            {"palisadeHeight", d.palisadeHeight},
            {"wallHeight", d.wallHeight},
            {"wallWidth", d.wallWidth},
            {"palisadeWidth", d.palisadeWidth},
            {"gateWidth", d.gateWidth},
            {"towerRadius", d.towerRadius},
            {"towerHeight", d.towerHeight},
            {"workUnitsPerSegment", d.workUnitsPerSegment},
            {"workUnitsPerGate", d.workUnitsPerGate},
            {"workUnitsPerTower", d.workUnitsPerTower},
            {"workUnitsPerBuilderPerMonth", d.workUnitsPerBuilderPerMonth},
            {"idleMonthsBeforeSuspension", d.idleMonthsBeforeSuspension}}}};
}

bool cityPoliciesFromJson(const json &j, CityPolicies &p, std::string *error)
{
  try
  {
    p = CityPolicies{};

    for (const auto &[key, value] : j.at("households").at("housingCapacity").items())
    {
      p.households.housingCapacity[HistoryNode(std::stoul(key))] = value.get<uint32_t>();
    }

    if (!j.at("households").at("independenceAgeYears").is_null())
    {
      p.households.independenceAgeYears = j.at("households").at("independenceAgeYears").get<uint32_t>();
    }

    const auto &e = j.at("economy");

    for (uint32_t o = 0; o < std::min<size_t>(cityOccupationCount, e.at("occupations").size()); ++o)
    {
      const auto &src = e.at("occupations").at(o);
      auto &dst = p.economy.occupations[o];

      dst.skillRequired = src.at("skillRequired").get<uint32_t>();
      dst.wagePerMonth = src.at("wagePerMonth").get<int64_t>();
      dst.inputsPerWorker = stockOf(src.at("inputsPerWorker"));
      dst.outputsPerWorker = stockOf(src.at("outputsPerWorker"));
      dst.publicService = src.at("publicService").get<bool>();
      dst.needsLand = src.at("needsLand").get<bool>();
      dst.needsTrade = src.at("needsTrade").get<bool>();
      dst.clergyOnly = src.at("clergyOnly").get<bool>();
    }

    for (uint32_t g = 0; g < cityGoodCount; ++g)
    {
      p.economy.prices[g] = e.at("prices").at(g).get<int64_t>();
    }

    p.economy.foodPerPersonPerMonth = e.at("foodPerPersonPerMonth").get<int64_t>();
    p.economy.subsistenceFoodPerPersonPerMonth = e.at("subsistenceFoodPerPersonPerMonth").get<int64_t>();
    p.economy.upkeepPerHouseholdPerMonth = e.at("upkeepPerHouseholdPerMonth").get<int64_t>();
    p.economy.upkeepPerPersonPerMonth = e.value("upkeepPerPersonPerMonth", int64_t(1));
    p.economy.householdSpendingPerMille = e.value("householdSpendingPerMille", uint32_t(100));
    p.economy.wealthTaxPerMille = e.value("wealthTaxPerMille", uint32_t(20));

    if (e.contains("treasurySeat"))
    {
      for (const auto &row : e.at("treasurySeat"))
      {
        p.economy.treasurySeat[row.at(0).get<HistoryNode>()] = row.at(1).get<HistoryNode>();
      }
    }
    p.economy.householdCoinReserve = e.value("householdCoinReserve", int64_t(40));
    p.economy.marketDuesPerMille = e.value("marketDuesPerMille", uint32_t(300));
    p.economy.storeFloatCoin = e.value("storeFloatCoin", int64_t(150));
    p.economy.taxPerMille = e.at("taxPerMille").get<uint32_t>();
    p.economy.monthWeeks = e.at("monthWeeks").get<uint32_t>();
    p.economy.reserveMonths = e.at("reserveMonths").get<uint32_t>();
    p.economy.householdGoodsReserve = e.at("householdGoodsReserve").get<int64_t>();
    p.economy.exportReserve = stockOf(e.at("exportReserve"));

    if (e.contains("importFloor"))
    {
      p.economy.importFloor = stockOf(e.at("importFloor"));
    }
    p.economy.apprenticesPerMentor = e.at("apprenticesPerMentor").get<uint32_t>();
    p.economy.mentorSkill = e.at("mentorSkill").get<uint32_t>();
    p.economy.maxSkill = e.at("maxSkill").get<uint32_t>();
    p.economy.retirementAge = e.at("retirementAge").get<uint32_t>();
    p.economy.tradeUnitsPerMerchant = e.at("tradeUnitsPerMerchant").get<int64_t>();

    for (const auto &r : e.at("routes"))
    {
      p.economy.routes.insert({r.at(0).get<HistoryNode>(), r.at(1).get<HistoryNode>()});
    }

    for (const auto &s : e.at("tradeOpen"))
    {
      p.economy.tradeOpen.insert(s.get<HistoryNode>());
    }

    for (const auto &[key, value] : e.at("harvestPerMille").items())
    {
      p.economy.harvestPerMille[uint32_t(std::stoul(key))] = value.get<uint32_t>();
    }

    p.economy.seed = e.at("seed").get<uint32_t>();

    const auto &s = j.at("succession");

    p.succession.majorityAge = s.at("majorityAge").get<uint32_t>();
    p.succession.regencyByConsort = s.at("regencyByConsort").get<bool>();
    p.succession.regencyByKin = s.at("regencyByKin").get<bool>();
    p.succession.electionFallback = s.at("electionFallback").get<bool>();

    const auto &pr = j.at("projects");

    for (uint32_t k = 0; k < cityProjectKindCount; ++k)
    {
      p.projects.costs[k] = stockOf(pr.at("costs").at(k));
    }

    p.projects.reviewWeeks = pr.at("reviewWeeks").get<uint32_t>();
    p.projects.marketHouseholds = pr.at("marketHouseholds").get<uint32_t>();
    p.projects.keepHouseholds = pr.at("keepHouseholds").get<uint32_t>();
    p.projects.wallHouseholds = pr.at("wallHouseholds").get<uint32_t>();
    p.projects.keepSafety = pr.at("keepSafety").get<double>();
    p.projects.wallSafety = pr.at("wallSafety").get<double>();

    const auto &d = j.at("defence");

    p.defence.segmentMetres = d.at("segmentMetres").get<double>();
    p.defence.towerSpacingMetres = d.at("towerSpacingMetres").get<double>();
    p.defence.marginMetres = d.at("marginMetres").get<double>();
    p.defence.palisadeHeight = d.at("palisadeHeight").get<double>();
    p.defence.wallHeight = d.at("wallHeight").get<double>();
    p.defence.wallWidth = d.at("wallWidth").get<double>();
    p.defence.palisadeWidth = d.at("palisadeWidth").get<double>();
    p.defence.gateWidth = d.at("gateWidth").get<double>();
    p.defence.towerRadius = d.at("towerRadius").get<double>();
    p.defence.towerHeight = d.at("towerHeight").get<double>();
    p.defence.workUnitsPerSegment = d.at("workUnitsPerSegment").get<uint32_t>();
    p.defence.workUnitsPerGate = d.at("workUnitsPerGate").get<uint32_t>();
    p.defence.workUnitsPerTower = d.at("workUnitsPerTower").get<uint32_t>();
    p.defence.workUnitsPerBuilderPerMonth = d.at("workUnitsPerBuilderPerMonth").get<uint32_t>();
    p.defence.idleMonthsBeforeSuspension = d.at("idleMonthsBeforeSuspension").get<uint32_t>();
  }
  catch (const std::exception &e)
  {
    return fail(error, std::string("policies: ") + e.what());
  }

  return true;
}

json cityScriptJson(const CityFoundingScript &s)
{
  json sites = json::object(), workplaces = json::array(), grants = json::array(), skills = json::array(), proposals = json::array();

  for (const auto &[settlement, f] : s.sites)
  {
    const auto &b = f.base;

    sites[std::to_string(settlement)] = {{"base",
                                         {{"seed", b.seed},
                                          {"tiers", b.tiers},
                                          {"width", b.width},
                                          {"tierDepth", b.tierDepth},
                                          {"tierRise", b.tierRise},
                                          {"lotFrontage", b.lotFrontage},
                                          {"alleyWidth", b.alleyWidth},
                                          {"terrain", int(b.terrain)},
                                          {"layout", int(b.layout)}}},
                                        {"doorstepMetres", f.doorstepMetres},
                                        {"attachMetres", f.attachMetres},
                                        {"minimumHeadroomMetres", f.minimumHeadroomMetres},
                                        {"workUnitsPerBuilderPerMonth", f.workUnitsPerBuilderPerMonth},
                                        {"idleMonthsBeforeSuspension", f.idleMonthsBeforeSuspension},
                                        {"repairAfterYears", f.repairAfterYears},
                                        {"rebuildAfterYears", f.rebuildAfterYears},
                                        {"membersPerFloor", f.membersPerFloor},
                                        {"footpathTraffic", f.footpathTraffic},
                                        {"footpathWeeks", f.footpathWeeks}};
  }

  for (const auto &w : s.workplaces)
  {
    workplaces.push_back({{"week", w.week}, {"settlement", w.spec.settlement}, {"occupation", int(w.spec.occupation)}, {"slots", w.spec.slots}, {"owner", node(w.spec.owner)}, {"landAccessible", w.spec.landAccessible}});
  }

  for (const auto &g : s.grants)
  {
    grants.push_back({{"week", g.week}, {"settlement", g.settlement}, {"household", g.household}, {"treasury", g.treasury}, {"good", int(g.good)}, {"amount", g.amount}});
  }

  for (const auto &k : s.skills)
  {
    skills.push_back({{"person", k.person}, {"occupation", int(k.occupation)}, {"level", k.level}});
  }

  for (const auto &[week, intent] : s.proposals)
  {
    proposals.push_back({{"week", week},
                         {"kind", int(intent.kind)},
                         {"settlement", intent.settlement},
                         {"sponsor", int(intent.sponsor)},
                         {"household", intent.household},
                         {"sponsorPerson", node(intent.sponsorPerson)},
                         {"reason", intent.reason},
                         {"priority", intent.priority},
                         {"site", intent.site ? json(*intent.site) : json(nullptr)}});
  }

  return {{"sites", sites}, {"workplaces", workplaces}, {"grants", grants}, {"skills", skills}, {"proposals", proposals}, {"foundingHouseholdCoin", s.foundingHouseholdCoin}, {"newcomerHouseholdCoin", s.newcomerHouseholdCoin}};
}

bool cityScriptFromJson(const json &j, CityFoundingScript &s, std::string *error)
{
  try
  {
    s = CityFoundingScript{};

    for (const auto &[key, value] : j.at("sites").items())
    {
      CityFabricSettings f;
      const auto &b = value.at("base");

      f.base.seed = b.at("seed").get<uint64_t>();
      f.base.tiers = b.at("tiers").get<uint32_t>();
      f.base.width = b.at("width").get<double>();
      f.base.tierDepth = b.at("tierDepth").get<double>();
      f.base.tierRise = b.at("tierRise").get<double>();
      f.base.lotFrontage = b.at("lotFrontage").get<double>();
      f.base.alleyWidth = b.at("alleyWidth").get<double>();
      f.base.terrain = CityBaseTerrain(b.at("terrain").get<int>());
      f.base.layout = CityBaseLayout(b.at("layout").get<int>());
      f.doorstepMetres = value.at("doorstepMetres").get<double>();
      f.attachMetres = value.at("attachMetres").get<double>();
      f.minimumHeadroomMetres = value.at("minimumHeadroomMetres").get<double>();
      f.workUnitsPerBuilderPerMonth = value.at("workUnitsPerBuilderPerMonth").get<uint32_t>();
      f.idleMonthsBeforeSuspension = value.at("idleMonthsBeforeSuspension").get<uint32_t>();
      f.repairAfterYears = value.at("repairAfterYears").get<uint32_t>();
      f.rebuildAfterYears = value.at("rebuildAfterYears").get<uint32_t>();
      f.membersPerFloor = value.at("membersPerFloor").get<uint32_t>();
      f.footpathTraffic = value.at("footpathTraffic").get<uint32_t>();
      f.footpathWeeks = value.at("footpathWeeks").get<uint32_t>();
      s.sites[HistoryNode(std::stoul(key))] = f;
    }

    for (const auto &w : j.at("workplaces"))
    {
      CityWorkplaceFounding f;

      f.week = w.at("week").get<uint32_t>();
      f.spec.settlement = w.at("settlement").get<HistoryNode>();
      f.spec.occupation = CityOccupation(w.at("occupation").get<int>());
      f.spec.slots = w.at("slots").get<uint32_t>();
      f.spec.owner = nodeOf(w.at("owner"));
      f.spec.landAccessible = w.at("landAccessible").get<bool>();
      s.workplaces.push_back(f);
    }

    for (const auto &g : j.at("grants"))
    {
      s.grants.push_back({g.at("week").get<uint32_t>(), g.at("settlement").get<HistoryNode>(), g.at("household").get<HouseholdId>(), g.at("treasury").get<bool>(), CityGood(g.at("good").get<int>()), g.at("amount").get<int64_t>()});
    }

    for (const auto &k : j.at("skills"))
    {
      s.skills.push_back({k.at("person").get<HistoryNode>(), CityOccupation(k.at("occupation").get<int>()), k.at("level").get<uint32_t>()});
    }

    for (const auto &p : j.at("proposals"))
    {
      CityProjectIntent intent;

      intent.kind = CityProjectKind(p.at("kind").get<int>());
      intent.settlement = p.at("settlement").get<HistoryNode>();
      intent.sponsor = CitySponsorKind(p.at("sponsor").get<int>());
      intent.household = p.at("household").get<HouseholdId>();
      intent.sponsorPerson = nodeOf(p.at("sponsorPerson"));
      intent.reason = p.at("reason").get<std::string>();
      intent.priority = p.at("priority").get<uint32_t>();

      if (!p.at("site").is_null())
      {
        intent.site = p.at("site").get<std::string>();
      }

      s.proposals.push_back({p.at("week").get<uint32_t>(), intent});
    }

    s.foundingHouseholdCoin = j.value("foundingHouseholdCoin", int64_t(0));
    s.newcomerHouseholdCoin = j.value("newcomerHouseholdCoin", int64_t(0));
  }
  catch (const std::exception &e)
  {
    return fail(error, std::string("script: ") + e.what());
  }

  return true;
}

// --- snapshots ----------------------------------------------------------------

json citySnapshotJson(const CityHistorySnapshot &s)
{
  json people = json::array(), rulers = json::array(), heirs = json::array(), consorts = json::array(), claimants = json::array(), vacant = json::array(), heads = json::array(), sworn = json::array(), feuds = json::array();

  for (const auto &[id, p] : s.people)
  {
    people.push_back({id, p.bornWeek, p.alive, node(p.settlement), node(p.dynasty), node(p.spouse), p.parents, int(p.stage), int(p.role)});
  }

  for (const auto &[realm, holder] : s.rulers)
  {
    rulers.push_back({realm, holder});
  }

  for (const auto &[realm, holder] : s.heirs)
  {
    heirs.push_back({realm, holder});
  }

  for (const auto &[realm, holder] : s.consorts)
  {
    consorts.push_back({realm, holder});
  }

  for (const auto &[realm, holders] : s.claimants)
  {
    claimants.push_back({realm, std::vector<HistoryNode>(holders.begin(), holders.end())});
  }

  for (auto realm : s.vacantRealms)
  {
    vacant.push_back(realm);
  }

  for (const auto &[house, head] : s.houseHeads)
  {
    heads.push_back({house, head});
  }

  for (const auto &[house, realm] : s.sworn)
  {
    sworn.push_back({house, realm});
  }

  for (const auto &[a, b] : s.feuds)
  {
    feuds.push_back({a, b});
  }

  return {{"week", s.week}, {"people", people}, {"rulers", rulers}, {"heirs", heirs}, {"consorts", consorts}, {"claimants", claimants}, {"vacant", vacant}, {"houseHeads", heads}, {"sworn", sworn}, {"feuds", feuds}};
}

bool citySnapshotFromJson(const json &j, CityHistorySnapshot &s, std::string *error)
{
  try
  {
    s = CityHistorySnapshot{};
    s.week = j.at("week").get<uint32_t>();

    for (const auto &row : j.at("people"))
    {
      CityHistoryPerson p;

      p.id = row.at(0).get<HistoryNode>();
      p.bornWeek = row.at(1).get<uint32_t>();
      p.alive = row.at(2).get<bool>();
      p.settlement = nodeOf(row.at(3));
      p.dynasty = nodeOf(row.at(4));
      p.spouse = nodeOf(row.at(5));
      p.parents = row.at(6).get<std::vector<HistoryNode>>();
      p.stage = CityLifeStage(row.at(7).get<int>());
      p.role = CitySocialRole(row.at(8).get<int>());
      s.people[p.id] = p;
    }

    for (const auto &row : j.at("rulers"))
    {
      s.rulers[row.at(0).get<HistoryNode>()] = row.at(1).get<HistoryNode>();
    }

    for (const auto &row : j.at("heirs"))
    {
      s.heirs[row.at(0).get<HistoryNode>()] = row.at(1).get<HistoryNode>();
    }

    for (const auto &row : j.at("consorts"))
    {
      s.consorts[row.at(0).get<HistoryNode>()] = row.at(1).get<HistoryNode>();
    }

    for (const auto &row : j.at("claimants"))
    {
      for (const auto &holder : row.at(1))
      {
        s.claimants[row.at(0).get<HistoryNode>()].insert(holder.get<HistoryNode>());
      }
    }

    for (const auto &realm : j.at("vacant"))
    {
      s.vacantRealms.insert(realm.get<HistoryNode>());
    }

    for (const auto &row : j.at("houseHeads"))
    {
      s.houseHeads[row.at(0).get<HistoryNode>()] = row.at(1).get<HistoryNode>();
    }

    for (const auto &row : j.at("sworn"))
    {
      s.sworn[row.at(0).get<HistoryNode>()] = row.at(1).get<HistoryNode>();
    }

    for (const auto &row : j.at("feuds"))
    {
      s.feuds.insert({row.at(0).get<HistoryNode>(), row.at(1).get<HistoryNode>()});
    }
  }
  catch (const std::exception &e)
  {
    return fail(error, std::string("snapshot: ") + e.what());
  }

  return true;
}

// --- the chronicle ---------------------------------------------------------------

CityChronicle::CityChronicle(CityPolicies policies, CityFoundingScript script) : rules(std::move(policies)), founding(std::move(script))
{
}

bool CityChronicle::apply(CityState &state, const CityHistorySnapshot &snapshot, std::string *error) const
{
  const bool first = !state.previous;
  const uint32_t week = snapshot.week;

  if (first)
  {
    CityHouseholdPolicy housing = rules.households;

    state.fabric = CityFabricLedger();

    for (const auto &[settlement, settings] : founding.sites)
    {
      if (!state.fabric.found(settlement, settings, week, cohortsAt(snapshot, settlement), error))
      {
        return false;
      }

      housing.housingCapacity[settlement] = state.fabric.facts(settlement).homes;
    }

    state.social = CityHistoryLedger();
    state.homes = CityHouseholdLedger(housing);
    state.economy = CityEconomyLedger(rules.economy);
    state.realm = CityRealmLedger(rules.succession, rules.projects);
    state.defence = CityDefenceLedger(rules.defence);
  }

  using clock = std::chrono::steady_clock;
  auto lap = [&](size_t part, clock::time_point &since) {
    const auto now = clock::now();

    seconds[part] += std::chrono::duration<double>(now - since).count();
    since = now;
  };
  auto since = clock::now();
  const auto weekStart = since;
  const auto observed = state.social.events().size();

  if (!state.social.observe(snapshot, error))
  {
    return false;
  }

  if (state.previous)
  {
    const std::vector<CityHistoryEvent> delta(state.social.events().begin() + long(observed), state.social.events().end());

    if (!cityPopulationReconciles(*state.previous, snapshot, delta, error))
    {
      return false;
    }
  }

  lap(0, since);

  if (!state.homes.observe(snapshot, error) || !state.homes.validate(snapshot, error))
  {
    return false;
  }

  lap(1, since);

  for (const auto &w : founding.workplaces)
  {
    if (w.week == week)
    {
      state.economy.openWorkplace(w.spec, week);
    }
  }

  for (const auto &g : founding.grants)
  {
    if (g.week != week)
    {
      continue;
    }

    if (g.household)
    {
      state.economy.grantHousehold(g.household, g.good, g.amount);
    }
    else if (g.treasury)
    {
      state.economy.grantTreasury(g.settlement, g.amount);
    }
    else
    {
      state.economy.grantStore(g.settlement, g.good, g.amount);
    }
  }

  if (!first && founding.newcomerHouseholdCoin > 0)
  {
    for (const auto &[hid, h] : state.homes.households())
    {
      if (!h.dissolvedWeek && h.foundedWeek == week && (h.kind == CityHouseholdOrigin::Unknown || h.kind == CityHouseholdOrigin::Migration))
      {
        state.economy.grantHousehold(hid, CityGood::Coin, founding.newcomerHouseholdCoin);
      }
    }
  }

  if (first)
  {
    for (const auto &k : founding.skills)
    {
      state.economy.grantSkill(k.person, k.occupation, k.level);
    }

    if (founding.foundingHouseholdCoin > 0)
    {
      for (const auto &[hid, h] : state.homes.households())
      {
        if (!h.dissolvedWeek)
        {
          state.economy.grantHousehold(hid, CityGood::Coin, founding.foundingHouseholdCoin);
        }
      }
    }
  }

  if (!state.economy.observe(snapshot, state.homes, error))
  {
    return false;
  }

  lap(2, since);

  for (const auto &[at, intent] : founding.proposals)
  {
    if (at == week)
    {
      state.realm.propose(intent, week);
    }
  }

  if (!state.realm.observe(snapshot, state.homes, state.economy, error))
  {
    return false;
  }

  lap(3, since);

  if (!state.fabric.observe(snapshot, state.homes, state.economy, state.realm, error))
  {
    return false;
  }

  lap(4, since);

  if (!state.defence.observe(snapshot, state.homes, state.economy, state.realm, state.fabric, error))
  {
    return false;
  }

  lap(5, since);
  slowest = std::max(slowest, std::chrono::duration<double>(clock::now() - weekStart).count());
  state.previous = snapshot;
  ++state.weeks;

  return true;
}

bool CityChronicle::observe(const CityHistorySnapshot &snapshot, std::string *error)
{
  if (recorded && uint64_t(snapshot.week) != uint64_t(firstWeek) + recorded)
  {
    return fail(error, "the chronicle records consecutive weeks");
  }

  CityState next = current;
  const size_t marks[] = {current.homes.eventMark(), current.economy.eventMark(), current.realm.eventMark(), current.fabric.eventMark(), current.defence.eventMark()};

  if (!apply(next, snapshot, error))
  {
    current.homes.truncateEvents(marks[0]);
    current.economy.truncateEvents(marks[1]);
    current.realm.truncateEvents(marks[2]);
    current.fabric.truncateEvents(marks[3]);
    current.defence.truncateEvents(marks[4]);

    return false;
  }

  if (!recorded)
  {
    firstWeek = snapshot.week;
  }

  ++recorded;

  if (keepStream)
  {
    weeks.push_back(snapshot);
  }

  current = std::move(next);

  if ((snapshot.week - firstWeek) % 52 == 0)
  {
    json row = {{"week", snapshot.week}, {"settlements", json::array()}};
    std::set<HistoryNode> places;

    for (const auto &[id, p] : snapshot.people)
    {
      if (p.alive && p.settlement)
      {
        places.insert(*p.settlement);
      }
    }

    for (auto place : places)
    {
      const auto economy = current.economy.facts(place, current.homes);
      const auto fabric = current.fabric.facts(place);
      uint32_t residents = 0, walls = 0;

      for (const auto &[id, p] : snapshot.people)
      {
        residents += p.alive && p.settlement == place;
      }

      const auto defence = current.defence.sites().find(place);

      if (defence != current.defence.sites().end())
      {
        for (const auto &c : defence->second.circuits)
        {
          walls += c.lifecycle == CityWallLifecycle::Standing;
        }
      }

      row["settlements"].push_back({{"settlement", place},
                                    {"residents", residents},
                                    {"households", current.homes.livingHouseholds(place)},
                                    {"employed", economy.employed},
                                    {"unemployed", economy.unemployed},
                                    {"foodStock", economy.foodStock},
                                    {"treasury", economy.treasury},
                                    {"homes", fabric.homes},
                                    {"wallsStanding", walls},
                                    {"stage", defence == current.defence.sites().end() ? "none" : cityDefenceStageName(defence->second.stage)}});
    }

    yearly.push_back(row);
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

bool CityChronicle::replay(std::optional<uint32_t> week, CityState &out, std::string *error) const
{
  if (!keepStream)
  {
    return fail(error, "the stream was not kept; nothing to replay");
  }

  CityChronicle fresh(rules, founding);

  for (const auto &snapshot : weeks)
  {
    if (week && snapshot.week > *week)
    {
      break;
    }

    if (!fresh.observe(snapshot, error))
    {
      return false;
    }
  }

  out = fresh.current;

  return true;
}

json CityChronicle::save() const
{
  json stream = json::array();

  for (const auto &s : weeks)
  {
    stream.push_back(citySnapshotJson(s));
  }

  return {{"schemaVersion", 1}, {"policies", cityPoliciesJson(rules)}, {"script", cityScriptJson(founding)}, {"weeks", stream}};
}

bool CityChronicle::load(const json &j, CityChronicle &out, std::string *error)
{
  CityPolicies policies;
  CityFoundingScript script;

  if (!cityPoliciesFromJson(j.at("policies"), policies, error) || !cityScriptFromJson(j.at("script"), script, error))
  {
    return false;
  }

  CityChronicle loaded(policies, script);

  for (const auto &row : j.at("weeks"))
  {
    CityHistorySnapshot snapshot;

    if (!citySnapshotFromJson(row, snapshot, error) || !loaded.observe(snapshot, error))
    {
      return false;
    }
  }

  out = std::move(loaded);

  return true;
}

uint64_t CityChronicle::hashOf(const CityState &s)
{
  std::ostringstream out;

  for (const auto &e : s.social.events())
  {
    out << e.week << cityHistoryEventName(e.kind) << e.subject << e.before.value_or(~0u) << e.after.value_or(~0u) << ';';
  }

  for (const auto &e : s.homes.events())
  {
    out << e.week << cityHouseholdEventName(e.kind) << e.household << e.person.value_or(~0u) << e.cause << ';';
  }

  for (const auto &e : s.economy.events())
  {
    out << e.week << cityEconomyEventName(e.kind) << e.person.value_or(~0u) << e.household << e.workplace << e.amount << ';';
  }

  for (const auto &e : s.realm.events())
  {
    out << e.week << cityRealmEventName(e.kind) << e.person.value_or(~0u) << e.asset << e.amount << e.reason << ';';
  }

  for (const auto &e : s.fabric.events())
  {
    out << e.week << cityFabricEventName(e.kind) << e.subject << e.household << e.detail << ';';
  }

  for (const auto &e : s.defence.events())
  {
    out << e.week << cityDefenceEventName(e.kind) << e.subject << e.detail << ';';
  }

  out << s.economy.coinInLedger() << ';' << s.homes.households().size() << ';' << s.weeks << ';';

  for (const auto &[settlement, site] : s.fabric.sites())
  {
    for (const auto &[id, b] : site.buildings)
    {
      for (const auto &v : b.versions)
      {
        out << id << ':' << v.version << ':' << v.fromWeek << ':' << v.toWeek.value_or(0) << ':' << v.geometryHash << ';';
      }
    }
  }

  return fnv(1469598103934665603ull, out.str());
}

json CityChronicle::at(uint32_t week) const
{
  json result = {{"week", week}, {"people", json::array()}, {"sites", json::array()}};

  if (weeks.empty() || week < weeks.front().week)
  {
    return result;
  }

  const auto &snapshot = weeks[std::min<size_t>(weeks.size() - 1, week - firstWeek)];

  for (const auto &[id, p] : snapshot.people)
  {
    if (p.alive)
    {
      result["people"].push_back({{"id", id}, {"settlement", node(p.settlement)}, {"stage", int(p.stage)}});
    }
  }

  for (const auto &[settlement, site] : current.fabric.sites())
  {
    if (site.foundedWeek > week)
    {
      continue;
    }

    json buildings = json::array(), routes = json::array(), walls = json::array();

    for (const auto &[id, v] : current.fabric.standingAt(settlement, week))
    {
      buildings.push_back({{"id", id}, {"version", v->version}, {"fromWeek", v->fromWeek}, {"use", cityBuildingUseName(site.buildings.at(id).use)}, {"floors", v->floors}, {"hash", v->geometryHash}});
    }

    for (const auto &[id, r] : site.routes)
    {
      if (r.openedWeek <= week && (!r.closedWeek || week < *r.closedWeek))
      {
        routes.push_back({{"id", id}, {"surface", cityRouteSurfaceName(r.surface)}});
      }
    }

    CityCompletionPlan plan;

    current.defence.applyTo(settlement, week, plan);

    for (const auto &s : plan.wallSegments)
    {
      walls.push_back({{"id", s.id}, {"wall", s.wall}, {"lifecycle", cityWallLifecycleName(s.lifecycle)}});
    }

    result["sites"].push_back({{"settlement", settlement}, {"buildings", buildings}, {"routes", routes}, {"wallSegments", walls}, {"gates", plan.gatehouses.size()}, {"towers", plan.towers.size()}});
  }

  return result;
}

json CityChronicle::inspectPerson(HistoryNode id) const
{
  if (!current.previous || !current.previous->people.count(id))
  {
    return nullptr;
  }

  const auto &snapshot = *current.previous;
  const auto &p = snapshot.people.at(id);
  json children = json::array(), offices = json::array();

  for (const auto &[other, q] : snapshot.people)
  {
    if (std::binary_search(q.parents.begin(), q.parents.end(), id))
    {
      children.push_back(other);
    }
  }

  for (const auto &[realm, c] : current.realm.realms())
  {
    if (c.ruler == id)
    {
      offices.push_back({{"realm", realm}, {"office", "ruler"}});
    }

    if (c.consort == id)
    {
      offices.push_back({{"realm", realm}, {"office", "consort"}});
    }

    if (c.heir == id)
    {
      offices.push_back({{"realm", realm}, {"office", "heir"}});
    }

    if (c.regent == id)
    {
      offices.push_back({{"realm", realm}, {"office", "regent"}});
    }

    if (c.claimants.count(id))
    {
      offices.push_back({{"realm", realm}, {"office", "claimant"}});
    }
  }

  const auto household = current.homes.householdOf(id);
  json home = nullptr, work = nullptr;

  if (household && p.settlement)
  {
    const auto site = current.fabric.sites().find(*p.settlement);

    if (site != current.fabric.sites().end())
    {
      for (const auto &[bid, b] : site->second.buildings)
      {
        if (b.tenant == household)
        {
          home = bid;
        }
      }
    }
  }

  const auto record = current.economy.livelihoods().find(id);

  if (record != current.economy.livelihoods().end())
  {
    const auto &r = record->second;
    json employer = nullptr;

    if (r.workplace && current.economy.workplaces().count(*r.workplace))
    {
      employer = node(current.economy.workplaces().at(*r.workplace).owner);
    }

    work = {{"state", r.adult ? cityLivelihoodName(r.livelihood) : cityDependencyName(r.dependency)},
            {"occupation", r.occupation ? json(cityOccupationName(*r.occupation)) : json(nullptr)},
            {"workplace", r.workplace ? json(*r.workplace) : json(nullptr)},
            {"employer", employer},
            {"mentor", node(r.mentor)},
            {"socialClass", citySocialClassName(r.socialClass)},
            {"career",
             {{"stage", cityCareerStageName(r.career)},
              {"trade", r.trade ? json(cityOccupationName(*r.trade)) : json(nullptr)},
              {"mentors", r.mentors},
              {"taughtByParent", r.taughtByParent},
              {"apprenticedWeek", r.apprenticedWeek ? json(*r.apprenticedWeek) : json(nullptr)},
              {"graduatedWeek", r.graduatedWeek ? json(*r.graduatedWeek) : json(nullptr)},
              {"masteredWeek", r.masteredWeek ? json(*r.masteredWeek) : json(nullptr)},
              {"ambition", r.ambition ? json(cityOccupationName(*r.ambition)) : json(nullptr)},
              {"openedWeek", r.openedWeek ? json(*r.openedWeek) : json(nullptr)}}}};
  }

  return {{"id", id},
          {"bornWeek", p.bornWeek},
          {"alive", p.alive},
          {"settlement", node(p.settlement)},
          {"dynasty", node(p.dynasty)},
          {"parents", p.parents},
          {"children", children},
          {"spouse", node(p.spouse)},
          {"lifeStage", int(p.stage)},
          {"socialRole", int(p.role)},
          {"household", household ? json(*household) : json(nullptr)},
          {"home", home},
          {"livelihood", work},
          {"offices", offices}};
}

json CityChronicle::inspectBuilding(HistoryNode settlement, uint64_t id) const
{
  const auto site = current.fabric.sites().find(settlement);

  if (site == current.fabric.sites().end() || !site->second.buildings.count(id))
  {
    return nullptr;
  }

  const auto &b = site->second.buildings.at(id);
  json versions = json::array(), occupants = json::array(), workers = json::array();

  for (const auto &v : b.versions)
  {
    versions.push_back({{"version", v.version}, {"fromWeek", v.fromWeek}, {"toWeek", v.toWeek ? json(*v.toWeek) : json(nullptr)}, {"floors", v.floors}, {"change", v.change}, {"project", v.project}, {"hash", v.geometryHash}});
  }

  if (b.tenant)
  {
    const auto h = current.homes.households().find(*b.tenant);

    if (h != current.homes.households().end())
    {
      for (auto member : h->second.members)
      {
        occupants.push_back(member);
      }
    }
  }

  if (b.workplace && current.economy.workplaces().count(*b.workplace))
  {
    for (auto w : current.economy.workplaces().at(*b.workplace).workers)
    {
      workers.push_back(w);
    }
  }

  return {{"id", id},
          {"parcel", b.parcel},
          {"use", cityBuildingUseName(b.use)},
          {"versions", versions},
          {"tenant", b.tenant ? json(*b.tenant) : json(nullptr)},
          {"occupants", occupants},
          {"sponsorHousehold", b.sponsorHousehold ? json(*b.sponsorHousehold) : json(nullptr)},
          {"sponsorPerson", node(b.sponsorPerson)},
          {"builders", std::vector<HouseholdId>(b.builders.begin(), b.builders.end())},
          {"workplace", b.workplace ? json(*b.workplace) : json(nullptr)},
          {"workers", workers},
          {"ruin", b.ruin},
          {"retiredWeek", b.retiredWeek ? json(*b.retiredWeek) : json(nullptr)}};
}

json CityChronicle::explainProject(uint64_t id) const
{
  const auto *found = current.realm.project(id);

  if (!found)
  {
    return nullptr;
  }

  const auto &p = *found;
  json timeline = json::array();

  for (const auto &e : current.realm.events())
  {
    if (e.asset == id)
    {
      timeline.push_back({{"week", e.week}, {"event", cityRealmEventName(e.kind)}, {"reason", e.reason}});
    }
  }

  for (const auto &e : current.fabric.events())
  {
    if (e.subject == id && (e.kind == CityFabricEventKind::Waiting || e.kind == CityFabricEventKind::ConstructionStarted || e.kind == CityFabricEventKind::ConstructionCompleted || e.kind == CityFabricEventKind::ConstructionRejected || e.kind == CityFabricEventKind::ConstructionSuspended || e.kind == CityFabricEventKind::ConstructionResumed))
    {
      timeline.push_back({{"week", e.week}, {"event", cityFabricEventName(e.kind)}, {"reason", e.detail}});
    }
  }

  std::sort(timeline.begin(), timeline.end(), [](const json &a, const json &b) { return a.at("week").get<uint32_t>() < b.at("week").get<uint32_t>(); });

  return {{"id", id},
          {"kind", cityProjectKindName(p.intent.kind)},
          {"settlement", p.intent.settlement},
          {"sponsor", p.intent.sponsor == CitySponsorKind::Treasury ? "treasury" : "household"},
          {"household", p.intent.household},
          {"sponsorPerson", node(p.intent.sponsorPerson)},
          {"reason", p.intent.reason},
          {"priority", p.intent.priority},
          {"site", p.intent.site ? json(*p.intent.site) : json(nullptr)},
          {"state", cityProjectStateName(p.state)},
          {"blockedBy", p.blockedBy},
          {"cost", stockJson(p.cost)},
          {"escrow", stockJson(p.escrow)},
          {"spent", stockJson(p.spent)},
          {"proposedWeek", p.proposedWeek},
          {"fundedWeek", p.fundedWeek ? json(*p.fundedWeek) : json(nullptr)},
          {"startedWeek", p.startedWeek ? json(*p.startedWeek) : json(nullptr)},
          {"completedWeek", p.completedWeek ? json(*p.completedWeek) : json(nullptr)},
          {"closedWeek", p.closedWeek ? json(*p.closedWeek) : json(nullptr)},
          {"timeline", timeline}};
}

json CityChronicle::series() const
{
  return yearly;
}
} // namespace procedural::world::rewrite
