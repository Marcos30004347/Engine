#include "WorldCityEconomy.hpp"
#include <algorithm>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *error, const std::string &message)
{
  if (error)
  {
    *error = message;
  }

  return false;
}

uint64_t mix(uint64_t x)
{
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;

  return x ^ (x >> 31);
}

int64_t clampMove(int64_t wanted, int64_t available)
{
  return std::max<int64_t>(0, std::min(wanted, available));
}

double clamp01(double v)
{
  return std::min(1.0, std::max(0.0, v));
}

bool working(CityLivelihood l)
{
  return l == CityLivelihood::Employed || l == CityLivelihood::SelfEmployed;
}

CityStock units(CityGood good, int64_t amount)
{
  CityStock s;

  s[good] = amount;

  return s;
}
} // namespace

bool CityStock::covers(const CityStock &cost) const
{
  for (uint32_t i = 0; i < cityGoodCount; ++i)
  {
    if (units[i] < cost.units[i])
    {
      return false;
    }
  }

  return true;
}

CityStock &CityStock::operator+=(const CityStock &other)
{
  for (uint32_t i = 0; i < cityGoodCount; ++i)
  {
    units[i] += other.units[i];
  }

  return *this;
}

CityStock &CityStock::operator-=(const CityStock &other)
{
  for (uint32_t i = 0; i < cityGoodCount; ++i)
  {
    units[i] -= other.units[i];
  }

  return *this;
}

CityEconomyPolicy::CityEconomyPolicy()
{
  auto &o = occupations;
  auto set = [&](CityOccupation occupation, uint32_t skill, int64_t wage, CityStock in, CityStock out) {
    auto &p = o[uint32_t(occupation)];

    p.skillRequired = skill;
    p.wagePerMonth = wage;
    p.inputsPerWorker = in;
    p.outputsPerWorker = out;
  };

  set(CityOccupation::Farmer, 0, 5, {}, units(CityGood::Food, 16));
  o[uint32_t(CityOccupation::Farmer)].needsLand = true;
  set(CityOccupation::Labourer, 0, 6, {}, units(CityGood::Timber, 5));
  set(CityOccupation::Carpenter, 1, 8, units(CityGood::Timber, 2), units(CityGood::Tools, 1));
  set(CityOccupation::Mason, 1, 8, units(CityGood::Tools, 1), units(CityGood::Stone, 4));
  set(CityOccupation::Smith, 2, 10, units(CityGood::Timber, 3), units(CityGood::Tools, 3));
  set(CityOccupation::Merchant, 1, 10, {}, {});
  o[uint32_t(CityOccupation::Merchant)].needsTrade = true;
  set(CityOccupation::Innkeeper, 1, 8, units(CityGood::Food, 2), units(CityGood::Coin, 10));
  o[uint32_t(CityOccupation::Innkeeper)].needsTrade = true;
  set(CityOccupation::Guard, 0, 6, {}, {});
  o[uint32_t(CityOccupation::Guard)].publicService = true;
  set(CityOccupation::Cleric, 0, 5, {}, {});
  o[uint32_t(CityOccupation::Cleric)].publicService = true;
  o[uint32_t(CityOccupation::Cleric)].clergyOnly = true;
  set(CityOccupation::Administrator, 2, 8, {}, {});
  o[uint32_t(CityOccupation::Administrator)].publicService = true;
  set(CityOccupation::Domestic, 0, 4, {}, {});
  // Food, tools and stone are aggregate stocks; these service trades earn
  // external sales using the same accounted coin flow as the innkeeper.
  set(CityOccupation::Baker, 1, 7, units(CityGood::Food, 2), units(CityGood::Coin, 10));
  set(CityOccupation::Brewer, 1, 8, units(CityGood::Food, 3), units(CityGood::Coin, 12));
  set(CityOccupation::Weaver, 1, 8, units(CityGood::Tools, 1), units(CityGood::Coin, 12));
  set(CityOccupation::Potter, 1, 7, units(CityGood::Stone, 2), units(CityGood::Coin, 12));
  set(CityOccupation::Herbalist, 1, 7, {}, units(CityGood::Coin, 8));
  o[uint32_t(CityOccupation::Herbalist)].needsLand = true;
  set(CityOccupation::Scribe, 2, 9, units(CityGood::Timber, 1), units(CityGood::Coin, 12));
  for(auto trade:{CityOccupation::Baker,CityOccupation::Brewer,CityOccupation::Weaver,CityOccupation::Potter,CityOccupation::Herbalist,CityOccupation::Scribe})
    o[uint32_t(trade)].needsTrade = true;
  exportReserve[CityGood::Timber] = 20;
  exportReserve[CityGood::Stone] = 20;
  exportReserve[CityGood::Tools] = 5;
  importFloor[CityGood::Timber] = 40;
  importFloor[CityGood::Stone] = 40;
  importFloor[CityGood::Tools] = 10;
}

CityEconomyLedger::CityEconomyLedger(CityEconomyPolicy policy) : rules(std::move(policy))
{
}

void CityEconomyLedger::grantStore(HistoryNode settlement, CityGood good, int64_t amount)
{
  homeSettlements.insert(settlement);
  towns[settlement].store[good] += amount;

  if (good == CityGood::Coin)
  {
    sums.coinGranted += amount;
  }
}

HistoryNode CityEconomyLedger::treasuryOf(HistoryNode settlement) const
{
  const auto seat = rules.treasurySeat.find(settlement);

  return seat == rules.treasurySeat.end() ? settlement : seat->second;
}

void CityEconomyLedger::grantTreasury(HistoryNode settlement, int64_t coin)
{
  homeSettlements.insert(settlement);
  towns[treasuryOf(settlement)].treasury += coin;
  towns[settlement];
  sums.coinGranted += coin;
}

void CityEconomyLedger::grantHousehold(HouseholdId id, CityGood good, int64_t amount)
{
  purses[id].stock[good] += amount;

  if (good == CityGood::Coin)
  {
    sums.coinGranted += amount;
  }
}

uint64_t CityEconomyLedger::openWorkplace(const CityWorkplaceSpec &spec, uint32_t week)
{
  CityWorkplace w;

  w.id = workplaceIdTag | nextWorkplace++;
  w.settlement = spec.settlement;
  w.occupation = spec.occupation;
  w.slots = spec.slots;
  w.owner = spec.owner;
  w.landAccessible = spec.landAccessible;
  w.openedWeek = week;
  places.emplace(w.id, w);
  homeSettlements.insert(spec.settlement);
  towns[spec.settlement];
  log->push_back({week, CityEconomyEventKind::WorkplaceOpened, spec.owner, spec.settlement, 0, w.id, CityGood::Coin, spec.slots});

  return w.id;
}

bool CityEconomyLedger::retireWorkplace(uint64_t id, uint32_t week)
{
  const auto it = places.find(id);

  if (it == places.end() || it->second.retiredWeek)
  {
    return false;
  }

  it->second.retiredWeek = week;
  log->push_back({week, CityEconomyEventKind::WorkplaceRetired, {}, it->second.settlement, 0, id, CityGood::Coin, 0});

  return true;
}

bool CityEconomyLedger::transferOwnership(uint64_t id, std::optional<HistoryNode> owner, uint32_t week)
{
  const auto it = places.find(id);

  if (it == places.end())
  {
    return false;
  }

  it->second.owner = owner;
  log->push_back({week, CityEconomyEventKind::OwnerChanged, owner, it->second.settlement, 0, id, CityGood::Coin, 0});

  return true;
}

void CityEconomyLedger::markUnableToWork(HistoryNode person, bool unable)
{
  records[person].unable = unable;
}

void CityEconomyLedger::grantSkill(HistoryNode person, CityOccupation occupation, uint32_t level)
{
  records[person].skills[uint32_t(occupation)] = uint8_t(std::min(level, rules.maxSkill));
}

bool CityEconomyLedger::transferAccount(HouseholdId from, std::optional<HouseholdId> to, HistoryNode settlement, uint32_t week)
{
  const auto it = purses.find(from);

  if (it == purses.end() || (to && *to == from))
  {
    return false;
  }

  const CityStock stock = it->second.stock;

  it->second.stock = CityStock{};

  if (to)
  {
    purses[*to].stock += stock;
  }
  else
  {
    auto &town = towns[settlement];

    towns[treasuryOf(settlement)].treasury += stock[CityGood::Coin];

    for (const auto good : {CityGood::Food, CityGood::Timber, CityGood::Stone, CityGood::Tools})
    {
      town.store[good] += stock[good];
    }
  }

  log->push_back({week, CityEconomyEventKind::OwnerChanged, {}, settlement, to.value_or(0), 0, CityGood::Coin, stock[CityGood::Coin]});

  return true;
}

bool CityEconomyLedger::spendTreasury(HistoryNode settlement, const CityStock &cost)
{
  auto &town = towns[settlement];
  auto &purse = towns[treasuryOf(settlement)];
  CityStock available = town.store;

  available[CityGood::Coin] = purse.treasury;

  if (!available.covers(cost))
  {
    return false;
  }

  purse.treasury -= cost[CityGood::Coin];

  for (const auto good : {CityGood::Food, CityGood::Timber, CityGood::Stone, CityGood::Tools})
  {
    town.store[good] -= cost[good];
  }

  sums.coinSpentOutside += cost[CityGood::Coin];

  return true;
}

bool CityEconomyLedger::spendHousehold(HouseholdId id, const CityStock &cost)
{
  auto &purse = purses[id];

  if (!purse.stock.covers(cost))
  {
    return false;
  }

  purse.stock -= cost;
  sums.coinSpentOutside += cost[CityGood::Coin];

  return true;
}

void CityEconomyLedger::refundTreasury(HistoryNode settlement, const CityStock &stock)
{
  auto &town = towns[settlement];

  towns[treasuryOf(settlement)].treasury += stock[CityGood::Coin];

  for (const auto good : {CityGood::Food, CityGood::Timber, CityGood::Stone, CityGood::Tools})
  {
    town.store[good] += stock[good];
  }

  sums.coinSpentOutside -= stock[CityGood::Coin];
}

void CityEconomyLedger::refundHousehold(HouseholdId id, const CityStock &stock)
{
  purses[id].stock += stock;
  sums.coinSpentOutside -= stock[CityGood::Coin];
}

void CityEconomyLedger::payFromEscrow(HouseholdId id, int64_t coin, uint32_t week)
{
  if (coin <= 0)
  {
    return;
  }

  auto &purse = purses[id];

  purse.stock[CityGood::Coin] += coin;
  purse.incomeThisMonth += coin;
  sums.coinSpentOutside -= coin;
  log->push_back({week, CityEconomyEventKind::Wages, {}, {}, id, 0, CityGood::Coin, coin});
}

bool CityEconomyLedger::buyFromStore(HouseholdId id, HistoryNode settlement, const CityStock &goods, uint32_t week)
{
  auto &purse = purses[id];
  auto &town = towns[settlement];
  int64_t price = 0;

  for (uint32_t g = 0; g < cityGoodCount; ++g)
  {
    if (CityGood(g) == CityGood::Coin)
    {
      continue;
    }

    if (goods.units[g] < 0 || town.store.units[g] < goods.units[g])
    {
      return false;
    }

    price += goods.units[g] * rules.prices[g];
  }

  if (purse.stock[CityGood::Coin] < price)
  {
    return false;
  }

  for (uint32_t g = 0; g < cityGoodCount; ++g)
  {
    if (CityGood(g) != CityGood::Coin && goods.units[g] > 0)
    {
      town.store.units[g] -= goods.units[g];
      purse.stock.units[g] += goods.units[g];
      log->push_back({week, CityEconomyEventKind::Bought, {}, settlement, id, 0, CityGood(g), goods.units[g]});
    }
  }

  purse.stock[CityGood::Coin] -= price;
  town.store[CityGood::Coin] += price;

  return true;
}

std::vector<HistoryNode> CityEconomyLedger::workersAt(HistoryNode settlement, std::initializer_list<CityOccupation> trades) const
{
  std::vector<HistoryNode> crew;

  for (const auto &[id, w] : places)
  {
    if (w.retiredWeek || w.settlement != settlement || std::find(trades.begin(), trades.end(), w.occupation) == trades.end())
    {
      continue;
    }

    crew.insert(crew.end(), w.workers.begin(), w.workers.end());
  }

  std::sort(crew.begin(), crew.end());

  return crew;
}

bool CityEconomyLedger::reachable(HistoryNode from, HistoryNode to) const
{
  return from == to || rules.routes.count({from, to}) || rules.routes.count({to, from});
}

int64_t CityEconomyLedger::coinInLedger() const
{
  int64_t coin = 0;

  for (const auto &[id, p] : purses)
  {
    coin += p.stock[CityGood::Coin];
  }

  for (const auto &[id, t] : towns)
  {
    coin += t.store[CityGood::Coin] + t.treasury;
  }

  return coin;
}

// One week's transaction over a working copy of the ledger.
class CityEconomyLedger::Week
{
public:
  Week(CityEconomyLedger &ledger, const CityHistorySnapshot &snapshot, const CityHouseholdLedger &homes) : L(ledger), snapshot(snapshot), homes(homes), week(snapshot.week)
  {
  }

  void run()
  {
    sync();
    caregivers();
    releaseOverflow();
    selfEmploy();
    hire();
    apprentice();
    classes();

    const uint32_t elapsed = week - *L.firstWeek;

    if (elapsed && elapsed % L.rules.monthWeeks == 0)
    {
      budget();
      ambitions();
    }

    if (elapsed && elapsed % 52 == 0)
    {
      learn();
    }
  }

private:
  CityEconomyLedger &L;
  const CityHistorySnapshot &snapshot;
  const CityHouseholdLedger &homes;
  const uint32_t week;

  const CityHistoryPerson *person(HistoryNode id) const
  {
    const auto it = snapshot.people.find(id);

    return it == snapshot.people.end() ? nullptr : &it->second;
  }

  const CityOccupationProfile &profile(CityOccupation occupation) const
  {
    return L.rules.occupations[uint32_t(occupation)];
  }

  void add(CityEconomyEventKind kind, std::optional<HistoryNode> who, std::optional<HistoryNode> settlement, HouseholdId household, uint64_t workplace, CityGood good, int64_t amount)
  {
    L.log->push_back({week, kind, who, settlement, household, workplace, good, amount});
  }

  std::optional<HouseholdId> householdOf(HistoryNode id) const
  {
    return homes.householdOf(id);
  }

  uint32_t age(const CityHistoryPerson &p) const
  {
    return (week - p.bornWeek) / 52;
  }

  bool isAdult(const CityHistoryPerson &p) const
  {
    return p.stage == CityLifeStage::Adult || p.stage == CityLifeStage::Elder;
  }

  void releaseApprenticesOf(HistoryNode mentor);

  void releaseJob(HistoryNode id, CityLivelihoodRecord &r)
  {
    if (!r.workplace)
    {
      return;
    }

    auto &w = L.places.at(*r.workplace);

    if (r.adult)
    {
      w.workers.erase(std::remove(w.workers.begin(), w.workers.end(), id), w.workers.end());
    }
    else
    {
      w.apprentices.erase(id);
    }

    add(CityEconomyEventKind::Released, id, w.settlement, householdOf(id).value_or(0), w.id, CityGood::Coin, 0);
    r.workplace.reset();
    r.mentor.reset();
    r.occupation.reset();

    if (r.adult)
    {
      if (working(r.livelihood))
      {
        r.livelihood = CityLivelihood::Unemployed;
      }

      // A master who leaves for any reason takes no apprentice with him.
      releaseApprenticesOf(id);
    }
    else
    {
      r.dependency = CityDependency::Dependent;
    }
  }

  void sync()
  {
    for (auto it = L.records.begin(); it != L.records.end();)
    {
      const auto *p = person(it->first);

      if (!p || !p->alive || !p->settlement)
      {
        releaseJob(it->first, it->second);
        releaseApprenticesOf(it->first);
        it = L.records.erase(it);
      }
      else
      {
        ++it;
      }
    }

    for (const auto &[id, p] : snapshot.people)
    {
      if (!p.alive || !p.settlement)
      {
        continue;
      }

      auto inserted = L.records.emplace(id, CityLivelihoodRecord{});
      auto &r = inserted.first->second;
      const bool adult = isAdult(p);

      if (inserted.second)
      {
        r.adult = adult;

        if (adult)
        {
          // Founders and migrants bring one trade: tuning data, not history.
          static const CityOccupation trades[] = {CityOccupation::Farmer,   CityOccupation::Labourer,  CityOccupation::Carpenter, CityOccupation::Mason,   CityOccupation::Smith,
                                                  CityOccupation::Merchant, CityOccupation::Innkeeper, CityOccupation::Guard,     CityOccupation::Domestic};
          const auto h = mix(uint64_t(L.rules.seed) << 32 | id);

          r.skills[uint32_t(trades[h % 9])] = uint8_t(1 + (h >> 8) % 3);
        }
      }
      else if (adult && !r.adult)
      {
        if (r.dependency == CityDependency::Apprentice)
        {
          // Long enough under a master, and the youth leaves a journeyman
          // with the trade in his hands; cut short, he leaves as he came.
          const bool served = r.trade && r.apprenticedWeek && week - *r.apprenticedWeek >= L.rules.apprenticeshipYears * 52;

          if (served)
          {
            r.career = CityCareerStage::Journeyman;
            r.graduatedWeek = week;
            r.skills[uint32_t(*r.trade)] = uint8_t(std::max<uint32_t>(r.skills[uint32_t(*r.trade)], L.rules.mentorSkill));
          }
          else
          {
            r.career = CityCareerStage::None;
          }

          add(CityEconomyEventKind::Graduated, id, p.settlement, householdOf(id).value_or(0), r.workplace.value_or(0), CityGood::Coin, served ? 1 : 0);
          releaseJob(id, r);
        }

        r.adult = true;
        r.livelihood = CityLivelihood::Unemployed;
      }

      if (r.workplace)
      {
        const auto &w = L.places.at(*r.workplace);
        const bool mentorGone = !r.adult && (!r.mentor || std::find(w.workers.begin(), w.workers.end(), *r.mentor) == w.workers.end());

        if (w.retiredWeek || !L.reachable(*p.settlement, w.settlement) || mentorGone)
        {
          releaseJob(id, r);
        }
      }

      if (!adult)
      {
        continue;
      }

      if (r.unable && r.livelihood != CityLivelihood::UnableToWork)
      {
        releaseJob(id, r);
        r.livelihood = CityLivelihood::UnableToWork;
        add(CityEconomyEventKind::Unable, id, p.settlement, householdOf(id).value_or(0), 0, CityGood::Coin, 1);
      }
      else if (!r.unable && r.livelihood == CityLivelihood::UnableToWork)
      {
        r.livelihood = CityLivelihood::Unemployed;
        add(CityEconomyEventKind::Unable, id, p.settlement, householdOf(id).value_or(0), 0, CityGood::Coin, 0);
      }

      if (age(p) >= L.rules.retirementAge && r.livelihood != CityLivelihood::Retired && r.livelihood != CityLivelihood::UnableToWork)
      {
        releaseJob(id, r);
        r.livelihood = CityLivelihood::Retired;
        add(CityEconomyEventKind::Retired, id, p.settlement, householdOf(id).value_or(0), 0, CityGood::Coin, 0);
      }
    }
  }

  void caregivers()
  {
    for (const auto &[hid, h] : homes.households())
    {
      if (h.dissolvedWeek)
      {
        continue;
      }

      std::vector<HistoryNode> infants, adults;
      std::optional<HistoryNode> current;

      for (auto member : h.members)
      {
        const auto *p = person(member);
        const auto r = L.records.find(member);

        if (!p || r == L.records.end())
        {
          continue;
        }

        if (week - p->bornWeek < 5 * 52)
        {
          infants.push_back(member);
        }

        if (p->stage == CityLifeStage::Adult && !r->second.unable)
        {
          adults.push_back(member);
        }

        if (r->second.livelihood == CityLivelihood::Caregiver)
        {
          current = member;
        }
      }

      const bool needed = !infants.empty() && adults.size() >= 2;

      if (!needed)
      {
        if (current)
        {
          L.records.at(*current).livelihood = CityLivelihood::Unemployed;
          add(CityEconomyEventKind::Caregiving, current, h.settlement, hid, 0, CityGood::Coin, 0);
        }

        continue;
      }

      if (current)
      {
        continue;
      }

      std::optional<HistoryNode> chosen;

      for (auto adult : adults)
      {
        if (adult == h.head)
        {
          continue;
        }

        for (auto infant : infants)
        {
          const auto &parents = person(infant)->parents;

          if (std::binary_search(parents.begin(), parents.end(), adult))
          {
            chosen = adult;
          }
        }

        if (chosen)
        {
          break;
        }
      }

      if (chosen)
      {
        auto &r = L.records.at(*chosen);

        releaseJob(*chosen, r);
        r.livelihood = CityLivelihood::Caregiver;
        add(CityEconomyEventKind::Caregiving, chosen, h.settlement, hid, 0, CityGood::Coin, 1);
      }
    }
  }

  void releaseOverflow()
  {
    for (auto &[id, w] : L.places)
    {
      while (!w.workers.empty() && (w.retiredWeek || w.workers.size() > w.slots))
      {
        releaseJob(w.workers.back(), L.records.at(w.workers.back()));
      }

      if (w.retiredWeek)
      {
        while (!w.apprentices.empty())
        {
          releaseJob(w.apprentices.begin()->first, L.records.at(w.apprentices.begin()->first));
        }
      }
    }
  }

  bool solvent(const CityWorkplace &w) const
  {
    const auto home = residentOwner(w);

    if (!home || profile(w.occupation).publicService)
    {
      return true;
    }

    const auto purse = L.purses.find(*home);

    return purse != L.purses.end() && purse->second.stock[CityGood::Coin] >= profile(w.occupation).wagePerMonth;
  }

  bool eligible(HistoryNode id, const CityLivelihoodRecord &r, const CityHistoryPerson &p, const CityWorkplace &w) const
  {
    const auto &pr = profile(w.occupation);

    const bool sore = r.quitFrom == w.id && r.quitWeek && week - *r.quitWeek < 26;

    return !sore && !w.retiredWeek && w.workers.size() < w.slots && L.reachable(*p.settlement, w.settlement) && r.skills[uint32_t(w.occupation)] >= pr.skillRequired && (!pr.clergyOnly || p.role == CitySocialRole::Clergy) &&
           (!pr.needsLand || w.landAccessible) && std::find(w.workers.begin(), w.workers.end(), id) == w.workers.end();
  }

  void take(HistoryNode id, CityLivelihoodRecord &r, CityWorkplace &w, CityLivelihood how)
  {
    w.workers.push_back(id);
    r.workplace = w.id;
    r.occupation = w.occupation;
    r.livelihood = how;
    add(how == CityLivelihood::SelfEmployed ? CityEconomyEventKind::SelfEmployed : CityEconomyEventKind::Hired, id, w.settlement, householdOf(id).value_or(0), w.id, CityGood::Coin, profile(w.occupation).wagePerMonth);
  }

  void selfEmploy()
  {
    for (auto &[wid, w] : L.places)
    {
      if (!w.owner || w.retiredWeek)
      {
        continue;
      }

      const auto home = residentOwner(w);

      if (!home)
      {
        continue;
      }

      // The owner first, from wage work elsewhere; a man already at a shop
      // of his own stays there. Then the household's idle.
      {
        const auto it = L.records.find(*w.owner);
        const auto *p = person(*w.owner);
        const bool atOwn = it != L.records.end() && it->second.workplace && L.places.count(*it->second.workplace) && L.places.at(*it->second.workplace).owner == w.owner;

        if (it != L.records.end() && p && it->second.adult && !atOwn && it->second.workplace != wid && it->second.livelihood != CityLivelihood::Retired && it->second.livelihood != CityLivelihood::UnableToWork &&
            it->second.livelihood != CityLivelihood::Caregiver && w.workers.size() < w.slots)
        {
          releaseJob(*w.owner, it->second);

          if (eligible(*w.owner, it->second, *p, w))
          {
            take(*w.owner, it->second, w, CityLivelihood::SelfEmployed);
          }
        }
      }

      for (auto member : homes.households().at(*home).members)
      {
        const auto it = L.records.find(member);
        const auto *p = person(member);

        if (it == L.records.end() || !p || !it->second.adult || it->second.livelihood != CityLivelihood::Unemployed || !eligible(member, it->second, *p, w))
        {
          continue;
        }

        take(member, it->second, w, CityLivelihood::SelfEmployed);
      }
    }
  }

  void hire()
  {
    std::vector<std::pair<HistoryNode, HistoryNode>> seekers;

    for (const auto &[id, r] : L.records)
    {
      if (r.adult && r.livelihood == CityLivelihood::Unemployed)
      {
        seekers.push_back({*person(id)->settlement, id});
      }
    }

    std::sort(seekers.begin(), seekers.end());

    for (const auto &[settlement, id] : seekers)
    {
      auto &r = L.records.at(id);
      const auto &p = *person(id);
      CityWorkplace *best = nullptr;
      std::tuple<int, int, int64_t, uint64_t> bestKey;

      for (auto &[wid, w] : L.places)
      {
        if (!eligible(id, r, p, w) || !solvent(w))
        {
          continue;
        }

        // A priest serves before he farms; otherwise the best-paid work the
        // person is most skilled at, and the older workplace on a tie.
        const int calling = profile(w.occupation).clergyOnly && p.role == CitySocialRole::Clergy ? 0 : 1;
        // A journeyman goes back to his master's shop before any other.
        const bool mastersShop = !r.mentors.empty() && std::find(w.workers.begin(), w.workers.end(), r.mentors.back()) != w.workers.end();
        const std::tuple<int, int, int64_t, uint64_t> key{calling, mastersShop ? -10 : -int(r.skills[uint32_t(w.occupation)]), -profile(w.occupation).wagePerMonth, wid};

        if (!best || key < bestKey)
        {
          best = &w;
          bestKey = key;
        }
      }

      if (best)
      {
        take(id, r, *best, CityLivelihood::Employed);
      }
    }
  }

  uint32_t apprenticesOf(HistoryNode mentor, const CityWorkplace &w) const
  {
    uint32_t n = 0;

    for (const auto &[a, m] : w.apprentices)
    {
      n += m == mentor;
    }

    return n;
  }

  void apprentice()
  {
    for (auto &[id, r] : L.records)
    {
      const auto &p = *person(id);

      if (r.adult || p.stage != CityLifeStage::ApprenticeAge || r.dependency == CityDependency::Apprentice)
      {
        continue;
      }

      CityWorkplace *best = nullptr;
      std::optional<HistoryNode> mentor;
      std::tuple<int, int, uint64_t, HistoryNode> bestKey;

      for (auto &[wid, w] : L.places)
      {
        if (w.retiredWeek || !L.reachable(*p.settlement, w.settlement) || profile(w.occupation).clergyOnly)
        {
          continue;
        }

        for (auto worker : w.workers)
        {
          const auto &m = L.records.at(worker);
          const int skill = m.skills[uint32_t(w.occupation)];

          if (skill < int(L.rules.mentorSkill) || apprenticesOf(worker, w) >= L.rules.apprenticesPerMentor)
          {
            continue;
          }

          const bool parent = std::binary_search(p.parents.begin(), p.parents.end(), worker);
          const std::tuple<int, int, uint64_t, HistoryNode> key{parent ? 0 : 1, -skill, wid, worker};

          if (!best || key < bestKey)
          {
            best = &w;
            mentor = worker;
            bestKey = key;
          }
        }
      }

      if (best)
      {
        best->apprentices[id] = *mentor;
        r.workplace = best->id;
        r.mentor = mentor;
        r.occupation = best->occupation;
        r.dependency = CityDependency::Apprentice;

        // The first master sets the trade; a later one continues it.
        if (r.career == CityCareerStage::None || r.trade != best->occupation)
        {
          r.career = CityCareerStage::Apprentice;
          r.trade = best->occupation;
          r.apprenticedWeek = week;
        }

        r.mentors.push_back(*mentor);
        r.taughtByParent = r.taughtByParent || std::binary_search(p.parents.begin(), p.parents.end(), *mentor);
        add(CityEconomyEventKind::Apprenticed, id, best->settlement, householdOf(id).value_or(0), best->id, CityGood::Coin, std::binary_search(p.parents.begin(), p.parents.end(), *mentor) ? 1 : 0);
      }
    }
  }

  void classes()
  {
    std::set<HistoryNode> royalHouses;

    for (const auto &[realm, ruler] : snapshot.rulers)
    {
      if (const auto *p = person(ruler); p && p->dynasty)
      {
        royalHouses.insert(*p->dynasty);
      }
    }

    for (auto &[id, r] : L.records)
    {
      const auto &p = *person(id);
      CitySocialClass c = CitySocialClass::Peasant;

      if (p.role == CitySocialRole::Ruler || p.role == CitySocialRole::Consort || p.role == CitySocialRole::Heir || (p.dynasty && royalHouses.count(*p.dynasty)))
      {
        c = CitySocialClass::Noble;
      }
      else if (p.role == CitySocialRole::Clergy)
      {
        c = CitySocialClass::Clergy;
      }
      else if (r.occupation == CityOccupation::Merchant || r.occupation == CityOccupation::Innkeeper || r.occupation == CityOccupation::Administrator)
      {
        c = CitySocialClass::Burgher;
      }
      else if (r.occupation == CityOccupation::Carpenter || r.occupation == CityOccupation::Mason || r.occupation == CityOccupation::Smith || (r.occupation && *r.occupation >= CityOccupation::Baker))
      {
        c = CitySocialClass::Craft;
      }

      if (c != r.socialClass)
      {
        r.socialClass = c;
        add(CityEconomyEventKind::ClassChanged, id, p.settlement, householdOf(id).value_or(0), 0, CityGood::Coin, int64_t(c));
      }
    }
  }

  // An owner who lives at the workplace's settlement. An owner abroad, or
  // dead and not yet inherited, leaves the place running for the market.
  std::optional<HouseholdId> residentOwner(const CityWorkplace &w) const
  {
    if (!w.owner)
    {
      return std::nullopt;
    }

    const auto home = householdOf(*w.owner);

    if (!home)
    {
      return std::nullopt;
    }

    const auto h = homes.households().find(*home);

    if (h == homes.households().end() || h->second.dissolvedWeek || h->second.settlement != w.settlement)
    {
      return std::nullopt;
    }

    return home;
  }

  // The stock a workplace runs from: its owner's purse, or the settlement store.
  CityStock &sourceOf(const CityWorkplace &w, HouseholdId &household)
  {
    if (const auto home = residentOwner(w))
    {
      household = *home;

      return L.purses[*home].stock;
    }

    household = 0;

    return L.towns[w.settlement].store;
  }

  int64_t &payerOf(const CityWorkplace &w, HouseholdId &household)
  {
    household = 0;

    if (profile(w.occupation).publicService)
    {
      return L.towns[L.treasuryOf(w.settlement)].treasury;
    }

    if (const auto home = residentOwner(w))
    {
      household = *home;

      return L.purses[*home].stock[CityGood::Coin];
    }

    // A public workplace of any trade is the crown's: its goods go to the
    // market, the market's dues to the treasury, and the treasury pays.
    return L.towns[L.treasuryOf(w.settlement)].treasury;
  }

  bool adminAt(HistoryNode settlement) const
  {
    for (const auto &[id, w] : L.places)
    {
      if (w.settlement == settlement && w.occupation == CityOccupation::Administrator && !w.workers.empty())
      {
        return true;
      }
    }

    return false;
  }

  uint32_t employedAt(HistoryNode settlement, CityOccupation occupation) const
  {
    uint32_t n = 0;

    for (const auto &[id, w] : L.places)
    {
      if (w.settlement == settlement && w.occupation == occupation && !w.retiredWeek)
      {
        n += uint32_t(w.workers.size());
      }
    }

    return n;
  }

  void produce()
  {
    const uint32_t year = (week - *L.firstWeek) / 52;
    const auto harvest = L.rules.harvestPerMille.find(year);
    const int64_t yield = harvest == L.rules.harvestPerMille.end() ? 1000 : harvest->second;

    for (auto &[wid, w] : L.places)
    {
      const auto &pr = profile(w.occupation);

      if (w.retiredWeek || w.workers.empty() || pr.publicService || (pr.needsLand && !w.landAccessible) || (pr.needsTrade && !L.rules.tradeOpen.count(w.settlement)))
      {
        continue;
      }

      HouseholdId household = 0;
      auto &source = sourceOf(w, household);
      // An apprentice's free work: never a full worker, never paid.
      int64_t runs = int64_t(w.workers.size()) + int64_t(w.apprentices.size()) / int64_t(std::max(1u, L.rules.apprenticeHalfWork));

      for (uint32_t g = 0; g < cityGoodCount; ++g)
      {
        if (pr.inputsPerWorker.units[g] > 0)
        {
          runs = std::min(runs, source.units[g] / pr.inputsPerWorker.units[g]);
        }
      }

      if (runs <= 0)
      {
        continue;
      }

      for (uint32_t g = 0; g < cityGoodCount; ++g)
      {
        source.units[g] -= runs * pr.inputsPerWorker.units[g];

        // Food a trade uses is food consumed; coin a trade earns is coin from
        // outside, like an export.
        if (CityGood(g) == CityGood::Food)
        {
          L.sums.foodConsumed += runs * pr.inputsPerWorker.units[g];
        }

        int64_t out = runs * pr.outputsPerWorker.units[g];

        if (w.occupation == CityOccupation::Farmer)
        {
          out = out * yield / 1000;
        }

        if (out > 0)
        {
          source.units[g] += out;

          if (CityGood(g) == CityGood::Food)
          {
            L.sums.foodProduced += out;
          }

          if (CityGood(g) == CityGood::Coin)
          {
            L.sums.coinFromTrade += out;
          }

          add(CityEconomyEventKind::Produced, w.owner, w.settlement, household, wid, CityGood(g), out);
        }
      }
    }
  }

  void wages()
  {
    std::vector<HistoryNode> quitting;

    for (auto &[wid, w] : L.places)
    {
      const int64_t wage = profile(w.occupation).wagePerMonth;

      for (auto worker : w.workers)
      {
        auto &r = L.records.at(worker);

        if (r.livelihood != CityLivelihood::Employed)
        {
          continue;
        }

        HouseholdId employer = 0;
        auto &coin = payerOf(w, employer);
        const auto home = householdOf(worker);
        const int64_t paid = clampMove(wage, coin);

        coin -= paid;

        if (home)
        {
          auto &purse = L.purses[*home];

          purse.stock[CityGood::Coin] += paid;
          purse.incomeThisMonth += paid;

          if (paid < wage)
          {
            purse.unpaidWages += wage - paid;
          }
        }

        if (paid > 0)
        {
          add(CityEconomyEventKind::Wages, worker, w.settlement, home.value_or(0), wid, CityGood::Coin, paid);
        }

        if (paid < wage)
        {
          if (!employer)
          {
            L.towns[w.settlement].unpaidPublicWages += wage - paid;
          }
          else
          {
            // A hand a household cannot pay leaves for other work.
            r.quitFrom = wid;
            r.quitWeek = week;
            quitting.push_back(worker);
          }

          add(CityEconomyEventKind::UnpaidWages, worker, w.settlement, home.value_or(0), wid, CityGood::Coin, wage - paid);
        }
      }
    }

    for (auto worker : quitting)
    {
      releaseJob(worker, L.records.at(worker));
    }
  }

  void consume()
  {
    for (const auto &[hid, h] : homes.households())
    {
      if (h.dissolvedWeek || !h.settlement || !L.simulated(*h.settlement))
      {
        continue;
      }

      auto &purse = L.purses[hid];
      auto &town = L.towns[*h.settlement];
      const int64_t members = int64_t(h.members.size());
      const int64_t need = members * L.rules.foodPerPersonPerMonth;
      const int64_t price = L.rules.prices[uint32_t(CityGood::Food)];
      int64_t eaten = clampMove(need, purse.stock[CityGood::Food]);

      purse.stock[CityGood::Food] -= eaten;

      if (eaten < need)
      {
        const int64_t bought = clampMove(std::min(need - eaten, town.store[CityGood::Food]), price > 0 ? purse.stock[CityGood::Coin] / price : need);

        if (bought > 0)
        {
          town.store[CityGood::Food] -= bought;
          purse.stock[CityGood::Coin] -= bought * price;
          town.store[CityGood::Coin] += bought * price;
          eaten += bought;
          add(CityEconomyEventKind::Bought, {}, h.settlement, hid, 0, CityGood::Food, bought);
        }
      }

      // The store empty, a household buys from a neighbour with more than
      // its reserve, at the same price; the farmer's surplus feeds the town
      // whether or not the store had the coin to buy it first.
      if (eaten < need)
      {
        for (const auto &[otherId, other] : homes.households())
        {
          if (eaten >= need || purse.stock[CityGood::Coin] < price || otherId == hid || other.dissolvedWeek || other.settlement != h.settlement)
          {
            continue;
          }

          auto seller = L.purses.find(otherId);

          if (seller == L.purses.end())
          {
            continue;
          }

          const int64_t reserve = int64_t(other.members.size()) * L.rules.foodPerPersonPerMonth * L.rules.reserveMonths;
          const int64_t surplus = seller->second.stock[CityGood::Food] - reserve;
          const int64_t bought = clampMove(std::min(need - eaten, surplus), price > 0 ? purse.stock[CityGood::Coin] / price : 0);

          if (bought > 0)
          {
            seller->second.stock[CityGood::Food] -= bought;
            seller->second.stock[CityGood::Coin] += bought * price;
            seller->second.incomeThisMonth += bought * price;
            purse.stock[CityGood::Coin] -= bought * price;
            eaten += bought;
            add(CityEconomyEventKind::Bought, {}, h.settlement, hid, 0, CityGood::Food, bought);
          }
        }
      }

      if (eaten < need)
      {
        const int64_t ration = clampMove(members * L.rules.subsistenceFoodPerPersonPerMonth - eaten, town.store[CityGood::Food]);

        if (ration > 0)
        {
          town.store[CityGood::Food] -= ration;
          eaten += ration;
          add(CityEconomyEventKind::Subsistence, {}, h.settlement, hid, 0, CityGood::Food, ration);
        }
      }

      // The granary bare, the ration comes as charity from a neighbour's
      // surplus beyond twice its own reserve.
      if (eaten < members * L.rules.subsistenceFoodPerPersonPerMonth)
      {
        for (const auto &[otherId, other] : homes.households())
        {
          const int64_t short_ = members * L.rules.subsistenceFoodPerPersonPerMonth - eaten;

          if (short_ <= 0)
          {
            break;
          }

          if (otherId == hid || other.dissolvedWeek || other.settlement != h.settlement)
          {
            continue;
          }

          auto donor = L.purses.find(otherId);

          if (donor == L.purses.end())
          {
            continue;
          }

          const int64_t reserve = int64_t(other.members.size()) * L.rules.foodPerPersonPerMonth * L.rules.reserveMonths * 2;
          const int64_t given = clampMove(short_, donor->second.stock[CityGood::Food] - reserve);

          if (given > 0)
          {
            donor->second.stock[CityGood::Food] -= given;
            eaten += given;
            add(CityEconomyEventKind::Subsistence, {}, h.settlement, hid, 0, CityGood::Food, given);
          }
        }
      }

      L.sums.foodConsumed += eaten;

      if (eaten < need)
      {
        purse.foodShortfall += need - eaten;
        town.foodShortfall += need - eaten;

        if (eaten < members * L.rules.subsistenceFoodPerPersonPerMonth)
        {
          ++purse.hungryMonths;
          add(CityEconomyEventKind::Hunger, {}, h.settlement, hid, 0, CityGood::Food, need - eaten);
        }
      }
    }
  }

  void sell()
  {
    for (const auto &[hid, h] : homes.households())
    {
      if (h.dissolvedWeek || !h.settlement || !L.simulated(*h.settlement))
      {
        continue;
      }

      auto &purse = L.purses[hid];
      auto &town = L.towns[*h.settlement];
      const int64_t reserve = int64_t(h.members.size()) * L.rules.foodPerPersonPerMonth * L.rules.reserveMonths;

      for (const auto good : {CityGood::Food, CityGood::Timber, CityGood::Stone, CityGood::Tools})
      {
        const int64_t price = L.rules.prices[uint32_t(good)];
        const int64_t surplus = purse.stock[good] - (good == CityGood::Food ? reserve : L.rules.householdGoodsReserve);
        const int64_t sold = price > 0 ? clampMove(surplus, town.store[CityGood::Coin] / price) : 0;

        if (sold > 0)
        {
          purse.stock[good] -= sold;
          town.store[good] += sold;
          town.store[CityGood::Coin] -= sold * price;
          purse.stock[CityGood::Coin] += sold * price;
          purse.incomeThisMonth += sold * price;
          add(CityEconomyEventKind::Sold, {}, h.settlement, hid, 0, good, sold);
        }
      }
    }
  }

  void taxAndUpkeep()
  {
    for (const auto &[hid, h] : homes.households())
    {
      if (h.dissolvedWeek || !h.settlement || !L.simulated(*h.settlement))
      {
        continue;
      }

      auto &purse = L.purses[hid];
      auto &town = L.towns[*h.settlement];
      const int64_t rate = adminAt(*h.settlement) ? L.rules.taxPerMille : L.rules.taxPerMille / 2;
      const int64_t wealth = std::max<int64_t>(0, purse.stock[CityGood::Coin] - L.rules.householdCoinReserve) * L.rules.wealthTaxPerMille / 1000;
      const int64_t tax = clampMove(purse.incomeThisMonth * rate / 1000 + wealth, purse.stock[CityGood::Coin]);

      purse.stock[CityGood::Coin] -= tax;
      L.towns[L.treasuryOf(*h.settlement)].treasury += tax;
      purse.incomeThisMonth = 0;

      if (tax > 0)
      {
        add(CityEconomyEventKind::Tax, {}, h.settlement, hid, 0, CityGood::Coin, tax);
      }

      const int64_t savings = purse.stock[CityGood::Coin] - L.rules.householdCoinReserve;
      const int64_t spending = savings > 0 ? savings * L.rules.householdSpendingPerMille / 1000 : 0;
      const int64_t upkeep = clampMove(L.rules.upkeepPerHouseholdPerMonth + L.rules.upkeepPerPersonPerMonth * int64_t(h.members.size()) + spending, purse.stock[CityGood::Coin]);

      purse.stock[CityGood::Coin] -= upkeep;
      town.store[CityGood::Coin] += upkeep;

      if (upkeep > 0)
      {
        add(CityEconomyEventKind::Upkeep, {}, h.settlement, hid, 0, CityGood::Coin, upkeep);
      }
    }
  }

  void trade()
  {
    for (auto &[settlement, town] : L.towns)
    {
      if (!L.rules.tradeOpen.count(settlement))
      {
        continue;
      }

      int64_t capacity = int64_t(employedAt(settlement, CityOccupation::Merchant)) * L.rules.tradeUnitsPerMerchant;
      int64_t need = 0;

      for (const auto &[hid, h] : homes.households())
      {
        if (!h.dissolvedWeek && h.settlement == settlement)
        {
          need += int64_t(h.members.size()) * L.rules.foodPerPersonPerMonth;
        }
      }

      for (const auto good : {CityGood::Tools, CityGood::Stone, CityGood::Timber, CityGood::Food})
      {
        const int64_t reserve = good == CityGood::Food ? need * L.rules.reserveMonths : L.rules.exportReserve[good];
        const int64_t exported = clampMove(std::min(capacity, town.store[good] - reserve), town.store[good]);

        if (exported <= 0)
        {
          continue;
        }

        const int64_t coin = exported * L.rules.prices[uint32_t(good)];

        town.store[good] -= exported;
        town.store[CityGood::Coin] += coin;
        town.importedCoin += coin;
        L.sums.coinFromTrade += coin;
        capacity -= exported;

        if (good == CityGood::Food)
        {
          L.sums.foodExported += exported;
        }

        add(CityEconomyEventKind::Exported, {}, settlement, 0, 0, good, exported);
      }

      for (const auto good : {CityGood::Timber, CityGood::Stone, CityGood::Tools})
      {
        const int64_t price = L.rules.prices[uint32_t(good)];
        const int64_t wanted = L.rules.importFloor[good] - town.store[good];
        const int64_t imported = wanted > 0 && price > 0 ? clampMove(std::min(wanted, capacity), town.store[CityGood::Coin] / price) : 0;

        if (imported > 0)
        {
          town.store[good] += imported;
          town.store[CityGood::Coin] -= imported * price;
          town.exportedCoin += imported * price;
          L.sums.coinToTrade += imported * price;
          capacity -= imported;
          add(CityEconomyEventKind::Imported, {}, settlement, 0, 0, good, imported);
        }
      }

      if (town.store[CityGood::Food] < need && capacity > 0)
      {
        const int64_t price = L.rules.prices[uint32_t(CityGood::Food)];
        const int64_t imported = price > 0 ? clampMove(std::min(need - town.store[CityGood::Food], capacity), town.store[CityGood::Coin] / price) : 0;

        if (imported > 0)
        {
          town.store[CityGood::Food] += imported;
          town.store[CityGood::Coin] -= imported * price;
          town.exportedCoin += imported * price;
          L.sums.coinToTrade += imported * price;
          L.sums.foodImported += imported;
          add(CityEconomyEventKind::Imported, {}, settlement, 0, 0, CityGood::Food, imported);
        }
      }
    }
  }

  void dues()
  {
    for (auto &[settlement, town] : L.towns)
    {
      const int64_t surplus = town.store[CityGood::Coin] - L.rules.storeFloatCoin;
      const int64_t paid = surplus > 0 ? surplus * L.rules.marketDuesPerMille / 1000 : 0;

      if (paid > 0)
      {
        town.store[CityGood::Coin] -= paid;
        L.towns[L.treasuryOf(settlement)].treasury += paid;
        add(CityEconomyEventKind::Tax, {}, settlement, 0, 0, CityGood::Coin, paid);
      }
    }
  }

  void budget()
  {
    produce();
    wages();
    consume();
    sell();
    taxAndUpkeep();
    trade();
    dues();
  }

  void learn()
  {
    for (auto &[id, r] : L.records)
    {
      if (r.occupation && (working(r.livelihood) || r.dependency == CityDependency::Apprentice))
      {
        auto &skill = r.skills[uint32_t(*r.occupation)];

        skill = uint8_t(std::min<uint32_t>(L.rules.maxSkill, skill + 1));
      }

      // A journeyman with the skill and the years is a master.
      if (r.career == CityCareerStage::Journeyman && r.trade && r.graduatedWeek && week - *r.graduatedWeek >= L.rules.masterYears * 52 && r.skills[uint32_t(*r.trade)] > L.rules.mentorSkill)
      {
        r.career = CityCareerStage::Master;
        r.masteredWeek = week;
        add(CityEconomyEventKind::Mastered, id, person(id)->settlement, householdOf(id).value_or(0), 0, CityGood::Coin, 0);
      }
    }
  }

  bool owns(HistoryNode id) const
  {
    for (const auto &[wid, w] : L.places)
    {
      if (w.owner == id && !w.retiredWeek)
      {
        return true;
      }
    }

    return false;
  }

  // Trained and not his own master: he means to keep a shop of his trade.
  void ambitions()
  {
    for (auto &[id, r] : L.records)
    {
      const bool trained = r.career == CityCareerStage::Journeyman || r.career == CityCareerStage::Master;

      if (!r.adult || !trained || !r.trade || r.ambition || owns(id))
      {
        continue;
      }

      switch (*r.trade)
      {
      case CityOccupation::Guard:
      case CityOccupation::Cleric:
      case CityOccupation::Administrator:
      case CityOccupation::Domestic:
      case CityOccupation::Labourer:
        continue; // No shop to keep in these.
      default:
        break;
      }

      r.ambition = r.trade;
      add(CityEconomyEventKind::Ambition, id, person(id)->settlement, householdOf(id).value_or(0), 0, CityGood::Coin, int64_t(*r.trade));
    }

    // The shop opened: its founder leaves wage work to run it.
    for (auto &[wid, w] : L.places)
    {
      if (!w.owner || w.retiredWeek)
      {
        continue;
      }

      auto r = L.records.find(*w.owner);

      if (r == L.records.end() || !r->second.ambition || r->second.openedWeek || w.occupation != *r->second.ambition)
      {
        continue;
      }

      r->second.openedWeek = week;
      r->second.ambition.reset();
      add(CityEconomyEventKind::Opened, *w.owner, w.settlement, householdOf(*w.owner).value_or(0), wid, CityGood::Coin, 0);

      if (r->second.workplace && r->second.workplace != wid)
      {
        releaseJob(*w.owner, r->second);
      }
    }
  }
};

void CityEconomyLedger::Week::releaseApprenticesOf(HistoryNode mentor)
{
  for (auto &[id, r] : L.records)
  {
    if (r.mentor == mentor)
    {
      releaseJob(id, r);
    }
  }
}

bool CityEconomyLedger::observe(const CityHistorySnapshot &snapshot, const CityHouseholdLedger &homes, std::string *error)
{
  if (lastWeek && uint64_t(snapshot.week) != uint64_t(*lastWeek) + 1)
  {
    return fail(error, "economy observations must be consecutive weeks");
  }

  if (homes.week() != std::optional<uint32_t>(snapshot.week))
  {
    return fail(error, "economy follows the household ledger of the same week");
  }

  const size_t mark = log->size();
  CityEconomyLedger next = *this;

  if (!next.firstWeek)
  {
    next.firstWeek = snapshot.week;
  }

  next.lastWeek = snapshot.week;

  Week week(next, snapshot, homes);

  week.run();

  if (!next.validate(snapshot, homes, error))
  {
    log->resize(mark);

    return false;
  }

  *this = std::move(next);

  if (error)
  {
    error->clear();
  }

  return true;
}

bool CityEconomyLedger::validate(const CityHistorySnapshot &snapshot, const CityHouseholdLedger &homes, std::string *error) const
{
  if (lastWeek != std::optional<uint32_t>(snapshot.week) || homes.week() != lastWeek)
  {
    return fail(error, "economy validated against a different week");
  }

  std::set<HistoryNode> placed;

  for (const auto &[id, w] : places)
  {
    if (w.workers.size() > w.slots)
    {
      return fail(error, "a workplace employs more workers than it has slots");
    }

    if (w.retiredWeek && (!w.workers.empty() || !w.apprentices.empty()))
    {
      return fail(error, "a retired workplace still employs people");
    }

    for (auto worker : w.workers)
    {
      const auto r = records.find(worker);
      const auto *p = snapshot.people.count(worker) ? &snapshot.people.at(worker) : nullptr;

      if (!p || !p->alive || !p->settlement || r == records.end() || r->second.workplace != id || !r->second.adult || !placed.insert(worker).second)
      {
        return fail(error, "a worker is dead, absent, a child, or works twice");
      }

      if (!reachable(*p->settlement, w.settlement) || (rules.occupations[uint32_t(w.occupation)].clergyOnly && p->role != CitySocialRole::Clergy))
      {
        return fail(error, "a worker cannot reach their workplace or is not eligible for it");
      }
    }

    std::map<HistoryNode, uint32_t> mentored;

    for (const auto &[apprentice, mentor] : w.apprentices)
    {
      const auto r = records.find(apprentice);
      const auto m = records.find(mentor);

      if (r == records.end() || r->second.workplace != id || r->second.mentor != mentor || r->second.adult || r->second.dependency != CityDependency::Apprentice || !placed.insert(apprentice).second)
      {
        return fail(error, "an apprentice record disagrees with its workplace");
      }

      if (std::find(w.workers.begin(), w.workers.end(), mentor) == w.workers.end() || m == records.end() || m->second.skills[uint32_t(w.occupation)] < rules.mentorSkill || ++mentored[mentor] > rules.apprenticesPerMentor)
      {
        return fail(error, "an apprentice has no eligible mentor");
      }
    }
  }

  for (const auto &[id, p] : snapshot.people)
  {
    const auto r = records.find(id);

    if (!p.alive || !p.settlement)
    {
      if (r != records.end())
      {
        return fail(error, "the dead or absent keep a livelihood");
      }

      continue;
    }

    if (r == records.end())
    {
      return fail(error, "a living resident has no livelihood record");
    }

    const auto &rec = r->second;
    const bool adult = p.stage == CityLifeStage::Adult || p.stage == CityLifeStage::Elder;

    if (rec.adult != adult)
    {
      return fail(error, "a livelihood record disagrees with the person's life stage");
    }

    if (adult && working(rec.livelihood) != rec.workplace.has_value())
    {
      return fail(error, "employment without a workplace, or a workplace without employment");
    }

    if (!adult && (rec.dependency == CityDependency::Apprentice) != rec.workplace.has_value())
    {
      return fail(error, "an apprentice without a workplace, or a child at work");
    }

    if (rec.workplace && !placed.count(id))
    {
      return fail(error, "a record names a workplace that does not list the person");
    }
  }

  for (const auto &[id, purse] : purses)
  {
    for (auto units : purse.stock.units)
    {
      if (units < 0)
      {
        return fail(error, "a household stock went negative");
      }
    }
  }

  for (const auto &[id, town] : towns)
  {
    if (town.treasury < 0)
    {
      return fail(error, "a treasury went negative");
    }

    for (auto units : town.store.units)
    {
      if (units < 0)
      {
        return fail(error, "a store stock went negative");
      }
    }
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

CityEconomyFacts CityEconomyLedger::facts(HistoryNode settlement, const CityHouseholdLedger &homes) const
{
  CityEconomyFacts f;
  int64_t residents = 0;

  for (const auto &[hid, h] : homes.households())
  {
    if (h.dissolvedWeek || h.settlement != settlement)
    {
      continue;
    }

    residents += int64_t(h.members.size());

    const auto purse = purses.find(hid);

    if (purse != purses.end())
    {
      f.foodStock += purse->second.stock[CityGood::Food];
      f.householdCoin += purse->second.stock[CityGood::Coin];
    }

    for (auto member : h.members)
    {
      const auto r = records.find(member);

      if (r == records.end() || !r->second.adult)
      {
        f.apprentices += r != records.end() && r->second.dependency == CityDependency::Apprentice;

        continue;
      }

      const auto &rec = r->second;

      f.workingAge += rec.livelihood != CityLivelihood::Retired;
      f.employed += working(rec.livelihood);
      f.unemployed += rec.livelihood == CityLivelihood::Unemployed;
    }
  }

  for (const auto &[id, w] : places)
  {
    if (w.retiredWeek || w.settlement != settlement)
    {
      continue;
    }

    f.freeSlots += w.slots - uint32_t(w.workers.size());
    f.guards += w.occupation == CityOccupation::Guard ? uint32_t(w.workers.size()) : 0;
    f.merchants += w.occupation == CityOccupation::Merchant ? uint32_t(w.workers.size()) : 0;
    f.administrators += w.occupation == CityOccupation::Administrator ? uint32_t(w.workers.size()) : 0;
  }

  const auto town = towns.find(settlement);

  if (town != towns.end())
  {
    f.foodStock += town->second.store[CityGood::Food];
    f.storeCoin = town->second.store[CityGood::Coin];
  }

  const auto seat = towns.find(treasuryOf(settlement));

  f.treasury = seat == towns.end() ? 0 : seat->second.treasury;

  f.foodNeedPerMonth = residents * rules.foodPerPersonPerMonth;
  f.work = clamp01(double(f.freeSlots) / double(f.unemployed + 1));
  f.food = f.foodNeedPerMonth ? clamp01(double(f.foodStock) / double(f.foodNeedPerMonth * rules.reserveMonths)) : 1.0;

  return f;
}

const char *cityOccupationName(CityOccupation o)
{
  static const char *const names[] = {"farmer", "labourer", "carpenter", "mason", "smith", "merchant", "innkeeper", "guard", "cleric", "administrator", "domestic", "baker", "brewer", "weaver", "potter", "herbalist", "scribe"};

  return uint32_t(o) < cityOccupationCount ? names[uint32_t(o)] : "unknown";
}

const char *cityLivelihoodName(CityLivelihood l)
{
  static const char *const names[] = {"employed", "self-employed", "caregiver", "unemployed", "unable-to-work", "retired"};

  return uint32_t(l) < 6 ? names[uint32_t(l)] : "unknown";
}

const char *cityDependencyName(CityDependency d)
{
  return d == CityDependency::Apprentice ? "apprentice" : "dependent";
}

const char *citySocialClassName(CitySocialClass c)
{
  static const char *const names[] = {"peasant", "craft", "burgher", "clergy", "noble"};

  return uint32_t(c) < 5 ? names[uint32_t(c)] : "unknown";
}

const char *cityGoodName(CityGood g)
{
  static const char *const names[] = {"food", "timber", "stone", "tools", "coin"};

  return uint32_t(g) < cityGoodCount ? names[uint32_t(g)] : "unknown";
}

const char *cityCareerStageName(CityCareerStage c)
{
  static const char *const names[] = {"none", "apprentice", "journeyman", "master"};

  return uint32_t(c) < 4 ? names[uint32_t(c)] : "unknown";
}

const char *cityEconomyEventName(CityEconomyEventKind k)
{
  static const char *const names[] = {"workplace-opened", "workplace-retired", "hired", "self-employed", "released", "apprenticed", "graduated", "retired", "caregiving", "unable", "class-changed", "produced",
                                      "wages",            "unpaid-wages",      "bought", "sold",          "subsistence", "hunger",     "tax",       "upkeep",  "exported",   "imported", "owner-changed",
                                      "mastered",         "ambition",          "opened"};

  return uint32_t(k) < 26 ? names[uint32_t(k)] : "unknown";
}
} // namespace procedural::world::rewrite
