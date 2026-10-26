#include "WorldCityScale.hpp"
#include <chrono>

namespace procedural::world::rewrite {

CitySocietySettings cityScaleSociety(const CityScaleSettings &s)
{
  CitySocietySettings society;

  society.seed = s.seed;
  society.maximumFeudYears = s.enrichedHistory ? 4 : 1;
  society.maximumLiving = s.maximumLiving;

  if (!s.settlements.empty())
  {
    for (const auto &k : s.settlements)
    {
      society.settlements.push_back({k.node, k.households});
      if (k.population) society.foundingPopulations[k.node] = k.population;
    }
    society.realm = s.seat ? s.seat : s.settlements.front().node;
    society.birthsPerFertileCoupleYear = .2;
    society.immigrantHouseholdsPerYearAtFullPull = 8;
    society.feudsPerYear = .1;

    return society;
  }

  switch (s.mode)
  {
  case CityScaleMode::Village:
    society.settlements = {{100, 6}};
    society.birthsPerFertileCoupleYear = .16;
    society.immigrantHouseholdsPerYearAtFullPull = .5;
    society.waitingTolerance = 0; // A village takes a newcomer only into an empty house.
    break;
  case CityScaleMode::Town:
    society.settlements = {{100, 12}};
    society.birthsPerFertileCoupleYear = .10;
    society.adultDeathsPerYear = .02;
    society.elderDeathsPerYear = .14;
    society.emigrantHouseholdsPerYear = 1;
    society.immigrantHouseholdsPerYearAtFullPull = 0;
    break;
  case CityScaleMode::City:
    society.settlements = {{100, 15}, {200, 10}};
    society.birthsPerFertileCoupleYear = .2;
    society.immigrantHouseholdsPerYearAtFullPull = 8;
    society.feudsPerYear = .1;
    break;
  }

  return society;
}

CityPolicies cityScalePolicies(const CityScaleSettings &s)
{
  CityPolicies p;
  if(s.enrichedHistory) {
    // A poor harvest lasts two years, followed by a recovery crop. Repeat
    // only once a generation, leaving room for prosperity between crises.
    for(uint32_t year=8+s.seed%5;year<s.years;year+=17+s.seed%4) {
      p.economy.harvestPerMille[year]=550;
      p.economy.harvestPerMille[year+1]=750;
      p.economy.harvestPerMille[year+2]=1200;
    }
  }

  if (!s.settlements.empty())
  {
    const HistoryNode seat = s.seat ? s.seat : s.settlements.front().node;
    for (const auto &k : s.settlements)
    {
      p.economy.tradeOpen.insert(k.node);
      if (k.node != seat)
      {
        p.economy.treasurySeat[k.node] = seat;
      }
    }
    for (const auto &[a, b] : s.routes)
    {
      p.economy.routes.insert({std::min(a, b), std::max(a, b)});
    }
    p.economy.seed = s.seed;
    p.projects.marketHouseholds = 8;
    p.projects.keepHouseholds = 12;
    p.projects.wallHouseholds = 16;

    return p;
  }

  p.economy.tradeOpen = {100, 200};
  p.economy.routes.insert({100, 200});
  p.economy.treasurySeat[200] = 100;
  p.economy.seed = s.seed;

  if (s.mode == CityScaleMode::City)
  {
    p.projects.marketHouseholds = 8;
    p.projects.keepHouseholds = 12;
    p.projects.wallHouseholds = 16;
  }

  return p;
}

CityFoundingScript cityScaleScript(const CityScaleSettings &s, const CityHistorySnapshot &founding)
{
  CityFoundingScript script;
  std::map<HistoryNode, uint32_t> residents;
  std::map<HistoryNode, std::set<HistoryNode>> houses;

  for (const auto &[id, p] : founding.people)
  {
    if (p.alive && p.settlement)
    {
      ++residents[*p.settlement];

      if (p.dynasty)
      {
        houses[*p.settlement].insert(*p.dynasty);
      }
    }
  }

  uint32_t index = 0;

  for (const auto &[settlement, count] : residents)
  {
    CityFabricSettings site;

    site.base.seed = 41 + s.seed * 7 + index * 13;
    site.base.tiers = s.mode == CityScaleMode::City ? 8 : s.mode == CityScaleMode::Town ? 4 : 3;
    site.base.width = s.mode == CityScaleMode::City ? 240 : s.mode == CityScaleMode::Town ? 120 : 84;
    uint32_t role = 1;
    for (const auto &k : s.settlements)
    {
      if (k.node == settlement)
      {
        site.base.tiers = k.tiers;
        site.base.width = k.baseWidth;
        role = k.role;
      }
    }
    site.base.layout = index % 2 ? CityBaseLayout::Staggered : CityBaseLayout::Tapered;
    site.base.terrain = index % 3 == 0 ? CityBaseTerrain::Hillside : index % 3 == 1 ? CityBaseTerrain::Ridge : CityBaseTerrain::Valley;
    script.sites[settlement] = site;
    ++index;

    // Two houses in three farm their own land; the third keeps a workshop of
    // its own. Only the crown's yard, its watch and its clerk are public.
    const CityOccupation crafts[] = {CityOccupation::Carpenter, CityOccupation::Mason, CityOccupation::Smith, CityOccupation::Merchant, CityOccupation::Innkeeper};
    uint32_t nth = 0, craftIndex = 0;

    for (auto house : houses[settlement])
    {
      const auto head = founding.houseHeads.find(house);

      if (head == founding.houseHeads.end())
      {
        continue;
      }

      CityWorkplaceFounding own;

      own.week = founding.week;
      own.spec.settlement = settlement;
      own.spec.owner = head->second;

      if (role>0 && nth % (role==1?4:3) == (role==1?3:2))
      {
        const CityOccupation varied[]={CityOccupation::Baker,CityOccupation::Weaver,CityOccupation::Brewer,
          CityOccupation::Potter,CityOccupation::Herbalist,CityOccupation::Scribe,CityOccupation::Smith,CityOccupation::Merchant,
          CityOccupation::Carpenter,CityOccupation::Mason,CityOccupation::Innkeeper};
        own.spec.occupation = s.enrichedHistory ? varied[craftIndex++ % 11] : crafts[(nth / 3) % 5];
        own.spec.slots = 2;
        script.skills.push_back({head->second, own.spec.occupation, 2});
      }
      else
      {
        own.spec.occupation = CityOccupation::Farmer;
        own.spec.slots = 3;
      }

      script.workplaces.push_back(own);
      ++nth;
    }

    // Public work by role: a hamlet has a labourer or two and no clerk; a
    // town a watch and a steward; the capital a garrison, masons for the
    // crown's works and an administration.
    const uint32_t scale = std::max(1u, count / 24);
    const uint32_t guards = role >= 4 ? 3 * scale + 2 : role >= 2 ? scale + 1 : role >= 1 ? scale : 0;
    const uint32_t clerks = role >= 4 ? 3 : role >= 2 ? 1 : 0;
    const std::pair<CityOccupation, uint32_t> publicWork[] = {{CityOccupation::Labourer, 2 * scale}, {CityOccupation::Mason, role >= 2 ? scale + 1 : scale}, {CityOccupation::Guard, std::max(1u, guards)}, {CityOccupation::Administrator, std::max(1u, clerks)}};

    for (const auto &[occupation, slots] : publicWork)
    {
      CityWorkplaceFounding w;

      w.week = founding.week;
      w.spec.settlement = settlement;
      w.spec.occupation = occupation;
      w.spec.slots = slots;
      script.workplaces.push_back(w);
    }

    // Six months of food bridge planting, employment and the first harvest.
    // Stocks scale with residents instead of giving a capital a hamlet's tools.
    const int64_t provision = s.establishedSettlements ? std::max(1u, count / 40) : 1;
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Food, int64_t(count) * 4 * (s.establishedSettlements ? 6 : 4)});
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Coin, 60 + 4 * int64_t(count)});
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Timber, 80 * provision});
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Stone, 80 * provision});
    script.grants.push_back({founding.week, settlement, 0, false, CityGood::Tools, 8 * provision});
    script.grants.push_back({founding.week, settlement, 0, true, CityGood::Coin, 200 + 10 * int64_t(count)});
  }

  script.foundingHouseholdCoin = 25;
  script.newcomerHouseholdCoin = 40;

  return script;
}

bool runCityScale(const CityScaleSettings &s, CityScaleReport &report, std::unique_ptr<CityChronicle> *keep)
{
  using clock = std::chrono::steady_clock;

  report = CityScaleReport{};
  if (s.years > CityScaleMaximumYears || s.years == 0) {
    report.error = "scale years must be 1..2000";
    return false;
  }
  if (s.cancelled && s.cancelled()) { report.error = "world history cancelled"; return false; }
  uint64_t census = 0;
  for (const auto &site : s.settlements) {
    if (site.population && site.population < 2) { report.error = "founding census needs at least two residents"; return false; }
    census += site.population;
  }
  if (census > s.maximumLiving) { report.error = "founding population exceeds maximumLiving; raise the limit or request fewer settlements"; return false; }
  const auto started = clock::now();
  CitySocietyDriver society(cityScaleSociety(s));
  const auto policies = cityScalePolicies(s);
  const auto script = cityScaleScript(s, society.current());
  auto chronicle = std::make_unique<CityChronicle>(policies, script);

  chronicle->setStreamRetention(s.keepStream);
  report = CityScaleReport{};
  report.startingLiving = society.living();

  int64_t foodGranted = 0;

  for (const auto &g : script.grants)
  {
    foodGranted += !g.treasury && !g.household && g.good == CityGood::Food ? g.amount : 0;
  }

  std::string error;

  if (!chronicle->observe(society.current(), &error))
  {
    report.error = "founding: " + error;

    return false;
  }

  for (uint32_t week = 1; week <= s.years * 52; ++week)
  {
    if (s.cancelled && s.cancelled()) { report.error = "world history cancelled"; return false; }
    std::map<HistoryNode, CitySocietyPull> pulls;
    const auto &state = chronicle->state();

    for (const auto &[settlement, site] : state.fabric.sites())
    {
      const auto economy = state.economy.facts(settlement, state.homes);
      CitySocietyPull pull;

      pull.work = economy.work;
      pull.food = economy.food;
      pull.safety = state.realm.safety(settlement, *state.previous, state.economy, state.homes);
      pull.vacantHomes = state.fabric.facts(settlement).vacantHomes;
      pull.waitingHouseholds = uint32_t(state.homes.waitingList(settlement).size());
      pulls[settlement] = pull;
    }

    const auto &snapshot = society.step(pulls);

    if (!chronicle->observe(snapshot, &error))
    {
      report.error = "week " + std::to_string(week) + ": " + error;

      return false;
    }
    if (week % 52 == 0 && s.yearlyObserver && !s.yearlyObserver(week / 52, chronicle->state(), error))
    {
      report.error = "milestone: " + error;
      return false;
    }
  }

  const auto &state = chronicle->state();

  report.weeks = state.weeks;
  report.livingEverywhere = society.living();

  for (const auto &[id, p] : state.previous->people)
  {
    report.living += p.alive && p.settlement && state.economy.simulated(*p.settlement);
  }

  report.society = society.counts();
  report.ledgerSeconds = chronicle->timings();
  report.slowestWeekSeconds = chronicle->slowestWeekSeconds();
  report.totalSeconds = std::chrono::duration<double>(clock::now() - started).count();
  report.hash = chronicle->stateHash();
  report.events = {{"social", state.social.events().size()},   {"households", state.homes.events().size()}, {"economy", state.economy.events().size()},
                   {"realm", state.realm.events().size()},     {"fabric", state.fabric.events().size()},    {"defence", state.defence.events().size()}};
  report.eventBytes = state.social.events().size() * sizeof(CityHistoryEvent) + state.homes.events().size() * sizeof(CityHouseholdEvent) + state.economy.events().size() * sizeof(CityEconomyEvent) +
                      state.realm.events().size() * sizeof(CityRealmEvent) + state.fabric.events().size() * sizeof(CityFabricEvent) + state.defence.events().size() * sizeof(CityDefenceEvent);

  for (const auto &snapshot : chronicle->stream())
  {
    report.streamBytes += snapshot.people.size() * (sizeof(CityHistoryPerson) + sizeof(HistoryNode) * 2);
  }

  int64_t food = 0;

  for (const auto &[id, purse] : state.economy.accounts())
  {
    food += purse.stock[CityGood::Food];
  }

  for (const auto &[settlement, town] : state.economy.settlements())
  {
    food += town.store[CityGood::Food];
    report.treasury += town.treasury;
  }

  const auto &t = state.economy.totals();

  report.foodStock = food;
  report.foodBalanced = food == foodGranted + t.foodProduced - t.foodConsumed - t.foodExported + t.foodImported;
  report.coinConserved = state.economy.coinInLedger() == t.coinGranted + t.coinFromTrade - t.coinToTrade - t.coinSpentOutside;

  for (const auto &[hid, h] : state.homes.households())
  {
    if (!h.dissolvedWeek)
    {
      ++report.households;
      report.waitingHouseholds += h.housing == CityHousingState::Waiting;
    }
  }

  for (const auto &[hid, purse] : state.economy.accounts())
  {
    report.hungryHouseholdMonths += purse.hungryMonths;
    report.householdCoin += purse.stock[CityGood::Coin];
    report.unpaidWages += purse.unpaidWages;
  }

  for (const auto &[settlement, town] : state.economy.settlements())
  {
    report.storeCoin += town.store[CityGood::Coin];
  }

  for (const auto &[settlement, site] : state.fabric.sites())
  {
    const auto facts = state.economy.facts(settlement, state.homes);

    report.employed += facts.employed;
    report.unemployed += facts.unemployed;
  }

  for (const auto &[settlement, site] : state.fabric.sites())
  {
    const auto facts = state.fabric.facts(settlement);

    report.homes += facts.homes;
    report.freeParcels += facts.freeParcels;

    for (const auto &[id, b] : site.buildings)
    {
      report.buildings += b.standing() != nullptr;
    }

    const auto defence = state.defence.sites().find(settlement);

    if (defence != state.defence.sites().end())
    {
      report.stage += (report.stage.empty() ? "" : ",") + std::string(cityDefenceStageName(defence->second.stage));

      for (const auto &c : defence->second.circuits)
      {
        report.wallsStanding += c.lifecycle == CityWallLifecycle::Standing;
      }
    }
  }

  for (const auto &[settlement, site] : state.fabric.sites())
  {
    const auto economy = state.economy.facts(settlement, state.homes);
    const auto fabric = state.fabric.facts(settlement);
    const auto town = state.economy.settlements().find(settlement);
    uint32_t residents = 0, waiting = 0, hungry = 0;
    int64_t unpaid = 0;

    for (const auto &[id, p] : state.previous->people)
    {
      residents += p.alive && p.settlement == settlement;
    }

    for (const auto &[hid, h] : state.homes.households())
    {
      if (!h.dissolvedWeek && h.settlement == settlement)
      {
        waiting += h.housing == CityHousingState::Waiting;

        const auto purse = state.economy.accounts().find(hid);

        unpaid += purse == state.economy.accounts().end() ? 0 : purse->second.unpaidWages;
        hungry += purse == state.economy.accounts().end() ? 0 : purse->second.hungryMonths;
      }
    }

    report.settlements[settlement] = "residents " + std::to_string(residents) + " households " + std::to_string(state.homes.livingHouseholds(settlement)) + " waiting " + std::to_string(waiting) + " homes " + std::to_string(fabric.homes) +
                                     " vacant " + std::to_string(fabric.vacantHomes) + " employed " + std::to_string(economy.employed) + "/" + std::to_string(economy.workingAge) + " treasury " +
                                     std::to_string(town == state.economy.settlements().end() ? 0 : town->second.treasury) + " store coin " + std::to_string(economy.storeCoin) + " unpaid " + std::to_string(unpaid) +
                                     " terraces " + std::to_string(fabric.occupiedBlocks.size()) + " unemployed " + std::to_string(economy.unemployed) + " hungry-months " + std::to_string(hungry);
  }

  std::vector<const CityProject *> everyProject;

  for (const auto &[id, p] : state.realm.projects())
  {
    everyProject.push_back(&p);
  }

  for (const auto &[id, p] : state.realm.closedProjects())
  {
    everyProject.push_back(&p);
  }

  for (const auto *pp : everyProject)
  {
    const auto &p = *pp;

    report.openProposals += p.state == CityProjectState::Proposed;

    if (p.intent.sponsor == CitySponsorKind::Treasury)
    {
      ++report.crownProjects[std::string(cityProjectKindName(p.intent.kind)) + " " + cityProjectStateName(p.state) + (p.blockedBy.empty() ? "" : ": " + p.blockedBy.substr(0, 40))];
    }

    if (p.state == CityProjectState::Proposed || p.state == CityProjectState::Funded)
    {
      ++report.blocked[std::string(cityProjectKindName(p.intent.kind)) + (p.intent.sponsor == CitySponsorKind::Treasury ? " (crown): " : ": ") + p.blockedBy.substr(0, 48)];
    }
  }

  for (const auto &[settlement, town] : state.economy.settlements())
  {
    report.storeTimber += town.store[CityGood::Timber];
    report.storeStone += town.store[CityGood::Stone];
  }

  if (keep)
  {
    *keep = std::move(chronicle);
  }

  return true;
}

const char *cityScaleModeName(CityScaleMode m)
{
  return m == CityScaleMode::Village ? "village" : m == CityScaleMode::Town ? "town" : "city";
}
} // namespace procedural::world::rewrite
