#include "WorldCityRealm.hpp"
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

double clamp01(double v)
{
  return std::min(1.0, std::max(0.0, v));
}

std::string describe(const CityStock &s)
{
  std::string text;

  for (uint32_t g = 0; g < cityGoodCount; ++g)
  {
    if (s.units[g] != 0)
    {
      text += (text.empty() ? "" : " ") + std::to_string(s.units[g]) + ' ' + cityGoodName(CityGood(g));
    }
  }

  return text.empty() ? "nothing" : text;
}

CityStock cost(int64_t coin, int64_t timber = 0, int64_t stone = 0)
{
  CityStock s;

  s[CityGood::Coin] = coin;
  s[CityGood::Timber] = timber;
  s[CityGood::Stone] = stone;

  return s;
}

bool open(CityProjectState s)
{
  return s == CityProjectState::Proposed || s == CityProjectState::Reserved || s == CityProjectState::Funded || s == CityProjectState::Building || s == CityProjectState::Suspended;
}
} // namespace

CityProjectPolicy::CityProjectPolicy()
{
  costs[uint32_t(CityProjectKind::Home)] = cost(20, 10);
  costs[uint32_t(CityProjectKind::Shop)] = cost(30, 10);
  costs[uint32_t(CityProjectKind::Inn)] = cost(40, 15);
  costs[uint32_t(CityProjectKind::Workshop)] = cost(25, 10);
  costs[uint32_t(CityProjectKind::Farm)] = cost(15);
  costs[uint32_t(CityProjectKind::Market)] = cost(60, 20);
  costs[uint32_t(CityProjectKind::Church)] = cost(80, 0, 20);
  costs[uint32_t(CityProjectKind::Keep)] = cost(150, 20, 40);
  costs[uint32_t(CityProjectKind::Wall)] = cost(200, 0, 80);
  costs[uint32_t(CityProjectKind::Road)] = cost(40, 0, 10);
  costs[uint32_t(CityProjectKind::Stair)] = cost(50, 0, 20);
  costs[uint32_t(CityProjectKind::Repair)] = cost(10, 5);
  costs[uint32_t(CityProjectKind::Extension)] = cost(15, 8);
  costs[uint32_t(CityProjectKind::Rebuild)] = cost(25, 12);
  costs[uint32_t(CityProjectKind::Manor)] = cost(60, 25);
  costs[uint32_t(CityProjectKind::Palisade)] = cost(80, 40);
}

CityRealmLedger::CityRealmLedger(CitySuccessionPolicy successionPolicy, CityProjectPolicy projectPolicy) : succession(successionPolicy), planning(std::move(projectPolicy))
{
}

uint64_t CityRealmLedger::propose(const CityProjectIntent &intent, uint32_t week)
{
  CityProject p;

  p.id = projectIdTag | nextProject++;
  p.intent = intent;
  p.cost = planning.costs[uint32_t(intent.kind)];
  p.proposedWeek = week;
  works.emplace(p.id, p);
  log->push_back({week, CityRealmEventKind::ProjectProposed, {}, intent.settlement, intent.sponsorPerson, {}, {}, p.id, int64_t(intent.priority), intent.reason});

  return p.id;
}

class CityRealmLedger::Week
{
public:
  Week(CityRealmLedger &ledger, const CityHistorySnapshot &snapshot, const CityHouseholdLedger &homes, CityEconomyLedger &economy) : L(ledger), snapshot(snapshot), homes(homes), economy(economy), week(snapshot.week)
  {
  }

  void run()
  {
    offices();
    inheritance();
    cancelStale();

    const uint32_t elapsed = week - *L.firstWeek;

    if (elapsed && elapsed % L.planning.reviewWeeks == 0)
    {
      review();
    }

    remember();
  }

private:
  CityRealmLedger &L;
  const CityHistorySnapshot &snapshot;
  const CityHouseholdLedger &homes;
  CityEconomyLedger &economy;
  const uint32_t week;

  const CityHistoryPerson *person(HistoryNode id) const
  {
    const auto it = snapshot.people.find(id);

    return it == snapshot.people.end() ? nullptr : &it->second;
  }

  bool adult(HistoryNode id) const
  {
    const auto *p = person(id);

    return p && p->alive && (week - p->bornWeek) / 52 >= L.succession.majorityAge;
  }

  void add(CityRealmEventKind kind, std::optional<HistoryNode> realm, std::optional<HistoryNode> settlement, std::optional<HistoryNode> who, std::optional<HistoryNode> before, std::optional<HistoryNode> after, uint64_t asset, int64_t amount, std::string reason)
  {
    L.log->push_back({week, kind, realm, settlement, who, before, after, asset, amount, std::move(reason)});
  }

  void office(HistoryNode realm, std::optional<HistoryNode> &held, std::optional<HistoryNode> now, CityRealmEventKind taken, CityRealmEventKind released)
  {
    if (held == now)
    {
      return;
    }

    if (held)
    {
      add(released, realm, {}, held, held, now, 0, 0, "");
    }

    if (now)
    {
      add(taken, realm, {}, now, held, now, 0, 0, "");
    }

    held = now;
  }

  std::optional<HistoryNode> lookup(const std::map<HistoryNode, HistoryNode> &table, HistoryNode realm) const
  {
    const auto it = table.find(realm);

    return it == table.end() ? std::nullopt : std::optional<HistoryNode>(it->second);
  }

  void offices()
  {
    std::set<HistoryNode> realms;

    for (const auto &[realm, holder] : snapshot.rulers)
    {
      realms.insert(realm);
    }

    for (const auto &[realm, holder] : snapshot.heirs)
    {
      realms.insert(realm);
    }

    for (const auto &[realm, holder] : snapshot.consorts)
    {
      realms.insert(realm);
    }

    for (const auto &[realm, holders] : snapshot.claimants)
    {
      realms.insert(realm);
    }

    for (const auto &[house, realm] : snapshot.sworn)
    {
      realms.insert(realm);
    }

    for (auto realm : snapshot.vacantRealms)
    {
      realms.insert(realm);
    }

    for (const auto &[realm, c] : L.crowns)
    {
      realms.insert(realm);
    }

    for (auto realm : realms)
    {
      auto &c = L.crowns[realm];

      c.realm = realm;
      office(realm, c.ruler, lookup(snapshot.rulers, realm), CityRealmEventKind::Crowned, CityRealmEventKind::CrownReleased);
      office(realm, c.consort, lookup(snapshot.consorts, realm), CityRealmEventKind::ConsortTaken, CityRealmEventKind::ConsortReleased);
      office(realm, c.heir, lookup(snapshot.heirs, realm), CityRealmEventKind::HeirNamed, CityRealmEventKind::HeirReleased);

      std::set<HistoryNode> claims;

      if (const auto it = snapshot.claimants.find(realm); it != snapshot.claimants.end())
      {
        claims = it->second;
      }

      for (auto id : c.claimants)
      {
        if (!claims.count(id))
        {
          add(CityRealmEventKind::ClaimDropped, realm, {}, id, {}, {}, 0, 0, "");
        }
      }

      for (auto id : claims)
      {
        if (!c.claimants.count(id))
        {
          add(CityRealmEventKind::ClaimRaised, realm, {}, id, {}, {}, 0, 0, "");
        }
      }

      c.claimants = claims;
      c.vacant = snapshot.vacantRealms.count(realm) != 0;

      if (c.ruler)
      {
        const auto *r = person(*c.ruler);

        c.seat = r->settlement;
        c.house = r->dynasty;
      }

      authority(c);
    }
  }

  void authority(CityRealm &c)
  {
    if (c.ruler && adult(*c.ruler))
    {
      if (c.regent)
      {
        add(CityRealmEventKind::RegencyEnded, c.realm, c.seat, c.regent, c.regent, c.ruler, 0, 0, "adult ruler");
        c.regent.reset();
      }

      c.authority = CityAuthorityKind::Ruler;
      c.authorityReason = "adult ruler";

      return;
    }

    const bool minority = c.ruler.has_value();
    auto valid = [&](std::optional<HistoryNode> id) { return id && adult(*id) && id != c.ruler; };
    std::optional<HistoryNode> regent;
    const char *reason = nullptr;

    if (valid(c.regent))
    {
      regent = c.regent;
      reason = c.authorityReason;
    }
    else if (L.succession.regencyByConsort && valid(c.consort))
    {
      regent = c.consort;
      reason = minority ? "minority: the consort acts" : "vacancy: the consort acts";
    }

    if (!regent && L.succession.regencyByKin && c.house)
    {
      for (const auto &[id, p] : snapshot.people)
      {
        if (p.dynasty == c.house && valid(id) && (!regent || p.bornWeek < person(*regent)->bornWeek))
        {
          regent = id;
        }
      }

      if (regent)
      {
        reason = minority ? "minority: the eldest of the house acts" : "vacancy: the eldest of the house acts";
      }
    }

    if (!regent && L.succession.electionFallback && c.seat)
    {
      const CityHousehold *largest = nullptr;

      for (const auto &[id, h] : homes.households())
      {
        if (!h.dissolvedWeek && h.settlement == c.seat && valid(h.head) && (!largest || h.members.size() > largest->members.size()))
        {
          largest = &h;
        }
      }

      if (largest)
      {
        regent = largest->head;
        reason = minority ? "minority: the head of the largest household is elected" : "vacancy: the head of the largest household is elected";
      }
    }

    if (regent)
    {
      if (c.regent != regent)
      {
        add(CityRealmEventKind::RegencyBegun, c.realm, c.seat, regent, c.regent, regent, 0, 0, reason);
      }

      c.regent = regent;
      c.authority = CityAuthorityKind::Regent;
      c.authorityReason = reason;

      return;
    }

    if (c.regent)
    {
      add(CityRealmEventKind::RegencyEnded, c.realm, c.seat, c.regent, c.regent, {}, 0, 0, "regent lost");
      c.regent.reset();
    }

    reason = minority ? "minority: no one can act" : "vacancy: no one can act";

    if (c.authority != CityAuthorityKind::None || std::string(c.authorityReason) != reason)
    {
      add(CityRealmEventKind::NoAuthority, c.realm, c.seat, {}, {}, {}, 0, 0, reason);
    }

    c.authority = CityAuthorityKind::None;
    c.authorityReason = reason;
  }

  // Eldest living child, adults first; then spouse; then eldest sibling; then a parent.
  std::optional<HistoryNode> heirOf(HistoryNode dead) const
  {
    const auto *d = person(dead);

    if (!d)
    {
      return std::nullopt;
    }

    std::optional<HistoryNode> best;
    std::tuple<int, uint32_t, HistoryNode> bestKey;
    auto consider = [&](HistoryNode id, int rank) {
      const auto *p = person(id);

      if (!p || !p->alive || id == dead)
      {
        return;
      }

      const std::tuple<int, uint32_t, HistoryNode> key{rank * 2 + (p->stage == CityLifeStage::Adult || p->stage == CityLifeStage::Elder ? 0 : 1), p->bornWeek, id};

      if (!best || key < bestKey)
      {
        best = id;
        bestKey = key;
      }
    };

    for (const auto &[id, p] : snapshot.people)
    {
      if (std::binary_search(p.parents.begin(), p.parents.end(), dead))
      {
        consider(id, 0);
      }
    }

    if (best)
    {
      return best;
    }

    if (d->spouse)
    {
      consider(*d->spouse, 1);
    }

    if (best)
    {
      return best;
    }

    for (const auto &[id, p] : snapshot.people)
    {
      std::vector<HistoryNode> shared;

      std::set_intersection(d->parents.begin(), d->parents.end(), p.parents.begin(), p.parents.end(), std::back_inserter(shared));

      if (!shared.empty())
      {
        consider(id, 2);
      }
    }

    if (best)
    {
      return best;
    }

    for (auto parent : d->parents)
    {
      consider(parent, 3);
    }

    return best;
  }

  void inheritance()
  {
    for (auto dead : L.living)
    {
      const auto *p = person(dead);

      if (p && p->alive)
      {
        continue;
      }

      const auto heir = heirOf(dead);

      for (const auto &[id, w] : std::map<uint64_t, CityWorkplace>(economy.workplaces()))
      {
        if (w.owner != dead || w.retiredWeek)
        {
          continue;
        }

        economy.transferOwnership(id, heir, week);

        if (heir)
        {
          add(CityRealmEventKind::Inherited, {}, w.settlement, heir, dead, heir, id, 0, "workplace");
        }
        else
        {
          add(CityRealmEventKind::Escheated, {}, w.settlement, {}, dead, {}, id, 0, "workplace with no heir");
        }
      }
    }

    for (const auto &[hid, members] : L.lastMembers)
    {
      const auto it = homes.households().find(hid);

      if (it == homes.households().end() || it->second.dissolvedWeek != std::optional<uint32_t>(week))
      {
        continue;
      }

      const auto purse = economy.accounts().find(hid);

      if (purse == economy.accounts().end() || purse->second.stock == CityStock{})
      {
        continue;
      }

      std::optional<HouseholdId> to;
      std::optional<HistoryNode> heir;

      for (auto member : members)
      {
        heir = heirOf(member);

        if (heir && homes.householdOf(*heir))
        {
          to = homes.householdOf(*heir);

          break;
        }
      }

      const int64_t coin = purse->second.stock[CityGood::Coin];

      economy.transferAccount(hid, to, *it->second.settlement, week);

      if (to)
      {
        add(CityRealmEventKind::Inherited, {}, it->second.settlement, heir, {}, {}, hid, coin, "household estate");
      }
      else
      {
        add(CityRealmEventKind::Escheated, {}, it->second.settlement, {}, {}, {}, hid, coin, "household estate with no heir");
      }
    }
  }

  void close(CityProject &p, const std::string &reason)
  {
    if (p.state == CityProjectState::Funded || p.state == CityProjectState::Building || p.state == CityProjectState::Suspended)
    {
      L.refund(p, economy);
    }

    p.state = CityProjectState::Cancelled;
    p.closedWeek = week;
    p.blockedBy = reason;
    add(CityRealmEventKind::ProjectCancelled, {}, p.intent.settlement, p.intent.sponsorPerson, {}, {}, p.id, 0, reason);
  }

  void cancelStale()
  {
    std::vector<uint64_t> closed;

    for (auto &[id, p] : L.works)
    {
      if (!open(p.state) || p.intent.sponsor != CitySponsorKind::Household)
      {
        continue;
      }

      const auto home = homes.households().find(p.intent.household);

      if (home == homes.households().end() || home->second.dissolvedWeek)
      {
        close(p, "the sponsoring household is gone");
        closed.push_back(id);
      }
      else if (p.intent.kind == CityProjectKind::Home && home->second.housing != CityHousingState::Waiting && (p.state == CityProjectState::Proposed || p.state == CityProjectState::Funded))
      {
        close(p, "the household found a home");
        closed.push_back(id);
      }
    }

    for (auto id : closed)
    {
      L.close(id);
    }
  }

  bool hasOpenSite(const std::string &site) const
  {
    for (const auto &[id, p] : L.works)
    {
      if (p.intent.site == site)
      {
        return true;
      }
    }

    return false;
  }

  bool hasOpen(CityProjectKind kind, HistoryNode settlement, CitySponsorKind sponsor, HouseholdId household) const
  {
    for (const auto &[id, p] : L.works)
    {
      if (open(p.state) && p.intent.kind == kind && p.intent.settlement == settlement && p.intent.sponsor == sponsor && p.intent.household == household)
      {
        return true;
      }
    }

    return false;
  }

  uint32_t completedCount(CityProjectKind kind, HistoryNode settlement) const
  {
    const auto found = L.completedCounts->find({kind, settlement});
    return found == L.completedCounts->end() ? 0 : found->second;
  }

  bool built(CityProjectKind kind, HistoryNode settlement) const
  {
    return completedCount(kind, settlement) > 0;
  }

  bool funded(CityProjectKind kind, HistoryNode settlement) const
  {
    if (built(kind, settlement))
    {
      return true;
    }

    for (const auto &[id, p] : L.works)
    {
      if ((p.state == CityProjectState::Funded || p.state == CityProjectState::Building || p.state == CityProjectState::Suspended) && p.intent.kind == kind && p.intent.settlement == settlement)
      {
        return true;
      }
    }

    return false;
  }

  void householdRules()
  {
    for (const auto &[hid, h] : homes.households())
    {
      if (h.dissolvedWeek || !h.settlement || !h.head)
      {
        continue;
      }

      CityProjectIntent intent;

      intent.settlement = *h.settlement;
      intent.sponsor = CitySponsorKind::Household;
      intent.household = hid;
      intent.sponsorPerson = h.head;

      if (h.housing == CityHousingState::Waiting && !hasOpen(CityProjectKind::Home, *h.settlement, CitySponsorKind::Household, hid))
      {
        intent.kind = CityProjectKind::Home;
        intent.priority = 50 + (week - *h.waitingSince) / 4;
        intent.reason = "waiting for a home since week " + std::to_string(*h.waitingSince);
        L.propose(intent, week);
      }

      // Anyone trained in the house who means to keep a shop, and can pay
      // the better part of it, asks for it in the house's name.
      for (auto member : h.members)
      {
        const auto trained = economy.livelihoods().find(member);

        if (trained == economy.livelihoods().end() || !trained->second.ambition)
        {
          continue;
        }

        std::optional<CityProjectKind> shop;

        switch (*trained->second.ambition)
        {
        case CityOccupation::Merchant:
          shop = CityProjectKind::Shop;
          break;
        case CityOccupation::Innkeeper:
          shop = CityProjectKind::Inn;
          break;
        case CityOccupation::Farmer:
          shop = CityProjectKind::Farm;
          break;
        case CityOccupation::Baker:
      case CityOccupation::Brewer:
      case CityOccupation::Weaver:
      case CityOccupation::Potter:
      case CityOccupation::Herbalist:
      case CityOccupation::Scribe:
      case CityOccupation::Carpenter:
        case CityOccupation::Mason:
        case CityOccupation::Smith:
          shop = CityProjectKind::Workshop;
          break;
        default:
          break;
        }

        const std::string key = "shop-" + std::to_string(member);

        if (!shop || hasOpenSite(key))
        {
          continue;
        }

        // A shop needs custom: while the trade has idle slots in the
        // settlement, another shop of it would stand empty.
        bool idle = false;

        for (const auto &[wid, w] : economy.workplaces())
        {
          idle = idle || (!w.retiredWeek && w.settlement == *h.settlement && w.occupation == *trained->second.ambition && w.workers.size() < w.slots);
        }

        if (idle)
        {
          continue;
        }

        const auto purse = economy.accounts().find(hid);
        const auto &cost = L.planning.costs[uint32_t(*shop)];
        int64_t needed = cost[CityGood::Coin];

        for (uint32_t g = 0; g < cityGoodCount; ++g)
        {
          needed += CityGood(g) == CityGood::Coin ? 0 : cost.units[g] * economy.policy().prices[g];
        }

        if (purse == economy.accounts().end() || purse->second.stock[CityGood::Coin] * 10 < needed * 6)
        {
          continue;
        }

        CityProjectIntent own = intent;

        own.kind = *shop;
        own.sponsorPerson = member;
        own.priority = 28;
        own.site = key;
        own.reason = std::string("a ") + cityCareerStageName(trained->second.career) + " " + cityOccupationName(*trained->second.ambition) + " means to keep his own " + cityProjectKindName(*shop);
        L.propose(own, week);
      }

      const auto record = economy.livelihoods().find(*h.head);

      if (record == economy.livelihoods().end() || !record->second.occupation)
      {
        continue;
      }

      std::optional<CityProjectKind> trade;
      uint32_t priority = 30;

      switch (*record->second.occupation)
      {
      case CityOccupation::Merchant:
        trade = CityProjectKind::Shop;
        break;
      case CityOccupation::Innkeeper:
        trade = CityProjectKind::Inn;
        break;
      case CityOccupation::Baker:
      case CityOccupation::Brewer:
      case CityOccupation::Weaver:
      case CityOccupation::Potter:
      case CityOccupation::Herbalist:
      case CityOccupation::Scribe:
      case CityOccupation::Carpenter:
      case CityOccupation::Mason:
      case CityOccupation::Smith:
        trade = CityProjectKind::Workshop;
        priority = 25;
        break;
      default:
        break;
      }

      bool keeps = false;

      for (const auto &[wid, w] : economy.workplaces())
      {
        keeps = keeps || (w.owner == h.head && !w.retiredWeek && w.occupation == *record->second.occupation);
      }

      if (trade && !keeps && !hasOpen(*trade, *h.settlement, CitySponsorKind::Household, hid))
      {
        intent.kind = *trade;
        intent.priority = priority;
        intent.reason = std::string("the head is a ") + cityOccupationName(*record->second.occupation);
        L.propose(intent, week);
      }
    }
  }

  // The settlements a realm rules: its seat and every settlement where a
  // house sworn to it lives.
  std::set<HistoryNode> realmSettlements(const CityRealm &c) const
  {
    std::set<HistoryNode> places;

    if (c.seat)
    {
      places.insert(*c.seat);
    }

    for (const auto &[id, p] : snapshot.people)
    {
      if (p.alive && p.settlement && p.dynasty)
      {
        const auto sworn = snapshot.sworn.find(*p.dynasty);

        if (sworn != snapshot.sworn.end() && sworn->second == c.realm && economy.simulated(*p.settlement))
        {
          places.insert(*p.settlement);
        }
      }
    }

    return places;
  }

  void treasuryRules()
  {
    for (const auto &[realm, c] : L.crowns)
    {
      if (!c.seat || !c.actor())
      {
        continue;
      }

      for (const auto seat : realmSettlements(c))
      {
        settlementRules(c, seat);
      }
    }
  }

  void settlementRules(const CityRealm &c, HistoryNode seat)
  {
    {
      const auto actor = c.actor();
      const uint32_t households = homes.livingHouseholds(seat);
      const double safe = L.safety(seat, snapshot, economy, homes);
      const auto facts = economy.facts(seat, homes);
      bool feud = false;

      for (const auto &[id, h] : homes.households())
      {
        if (h.dissolvedWeek || h.settlement != seat || !h.head)
        {
          continue;
        }

        const auto *head = person(*h.head);

        for (const auto &[a, b] : snapshot.feuds)
        {
          feud = feud || (head && head->dynasty && (a == *head->dynasty || b == *head->dynasty));
        }
      }

      // One of each per seat, except walls, which come in eras.
      auto crown = [&](CityProjectKind kind, uint32_t priority, const char *site, std::string reason) {
        if (hasOpen(kind, seat, CitySponsorKind::Treasury, 0) || (kind != CityProjectKind::Wall && built(kind, seat)))
        {
          return;
        }

        CityProjectIntent intent;

        intent.kind = kind;
        intent.settlement = seat;
        intent.sponsor = CitySponsorKind::Treasury;
        intent.sponsorPerson = actor;
        intent.priority = priority;
        intent.site = site;
        intent.reason = std::move(reason);
        L.propose(intent, week);
      };

      if (households >= L.planning.marketHouseholds)
      {
        crown(CityProjectKind::Market, 40 + (facts.merchants ? 10 : 0) - (feud ? 10 : 0), "market", std::to_string(households) + " households need a market");
      }

      // Housing for those waiting, when the treasury can carry it.
      {
        uint32_t waiting = 0;

        for (const auto &[id, h] : homes.households())
        {
          waiting += !h.dissolvedWeek && h.settlement == seat && h.housing == CityHousingState::Waiting;
        }

        const auto town = economy.settlements().find(economy.treasuryOf(seat));
        const int64_t treasury = town == economy.settlements().end() ? 0 : town->second.treasury;
        const int64_t homeCoin = L.planning.costs[uint32_t(CityProjectKind::Home)][CityGood::Coin];

        uint32_t openHomes = 0;

        for (const auto &[id, p] : L.works)
        {
          openHomes += p.intent.kind == CityProjectKind::Home && p.intent.sponsor == CitySponsorKind::Treasury && p.intent.settlement == seat;
        }

        // As many at once as the waiting warrant and the treasury carries.
        const uint32_t wanted = waiting / L.planning.crownHousingWaiting;

        if (waiting >= L.planning.crownHousingWaiting && openHomes < wanted && treasury >= homeCoin * int64_t(L.planning.crownHousingReserveHomes + openHomes))
        {
          CityProjectIntent intent;

          intent.kind = CityProjectKind::Home;
          intent.settlement = seat;
          intent.sponsor = CitySponsorKind::Treasury;
          intent.sponsorPerson = actor;
          intent.priority = 45;
          intent.reason = std::to_string(waiting) + " households wait for a home";
          L.propose(intent, week);
        }

        // Work for the idle: a crown farm or workshop when too many stand idle.
        const auto facts = economy.facts(seat, homes);

        if (facts.unemployed >= L.planning.crownWorksIdle && treasury >= homeCoin * int64_t(L.planning.crownHousingReserveHomes) && !hasOpen(CityProjectKind::Farm, seat, CitySponsorKind::Treasury, 0) && !hasOpen(CityProjectKind::Workshop, seat, CitySponsorKind::Treasury, 0))
        {
          CityProjectIntent intent;

          const bool foodReserveLow = facts.foodStock < facts.foodNeedPerMonth * int64_t(economy.policy().reserveMonths);
          intent.kind = foodReserveLow ? CityProjectKind::Farm : CityProjectKind::Workshop;
          intent.settlement = seat;
          intent.sponsor = CitySponsorKind::Treasury;
          intent.sponsorPerson = actor;
          intent.priority = 42;
          intent.reason = std::to_string(facts.unemployed) + (foodReserveLow ? " stand idle; farms must replenish the food reserve" : " stand idle; food is secure and the town needs craft employment");
          L.propose(intent, week);
        }
      }

      // The seat's defences grow in order: hall, palisade, keep, curtain.
      crown(CityProjectKind::Manor, 30, "manor", "the seat needs a hall");

      if (completedCount(CityProjectKind::Manor, seat) && (households >= L.planning.marketHouseholds || safe < L.planning.wallSafety))
      {
        crown(CityProjectKind::Palisade, 40 + (safe < L.planning.keepSafety ? 20 : 0) + (feud ? 10 : 0), "palisade", safe < L.planning.keepSafety ? "threat: the hall wants a palisade" : "a grown seat fences its hall");
      }

      if (completedCount(CityProjectKind::Palisade, seat) && (households >= L.planning.keepHouseholds || safe < L.planning.keepSafety))
      {
        crown(CityProjectKind::Keep, 35 + (safe < L.planning.keepSafety ? 25 : 0) + (feud ? 10 : 0), "keep", safe < L.planning.keepSafety ? "threat: the seat is unsafe" : "prestige: the seat has grown");
      }

      if (funded(CityProjectKind::Keep, seat) && households >= L.planning.wallHouseholds && safe < L.planning.wallSafety)
      {
        const std::string era = "wall-" + std::to_string(completedCount(CityProjectKind::Wall, seat) + 1);

        crown(CityProjectKind::Wall, 45 + (feud ? 10 : 0), era.c_str(), completedCount(CityProjectKind::Wall, seat) ? "the suburbs outside the wall want a circuit of their own" : "a grown seat behind a keep wants a wall");
      }

      if (economy.policy().tradeOpen.count(seat) && facts.merchants)
      {
        crown(CityProjectKind::Road, 20, "road", "trade wants a road");
      }

      bool clergy = false, church = false;

      for (const auto &[id, p] : snapshot.people)
      {
        clergy = clergy || (p.alive && p.settlement == seat && p.role == CitySocialRole::Clergy);
      }

      for (const auto &[id, w] : economy.workplaces())
      {
        church = church || (!w.retiredWeek && w.settlement == seat && w.occupation == CityOccupation::Cleric);
      }

      if (clergy && !church)
      {
        crown(CityProjectKind::Church, 30, "church", "a priest with no church");
      }
    }
  }

  void resolve()
  {
    std::vector<std::tuple<int64_t, int, HistoryNode, int, HouseholdId, uint64_t>> order;

    for (const auto &[id, p] : L.works)
    {
      if (p.state == CityProjectState::Proposed)
      {
        order.push_back({-int64_t(p.intent.priority), p.intent.sponsor == CitySponsorKind::Treasury ? 0 : 1, p.intent.settlement, int(p.intent.kind), p.intent.household, id});
      }
    }

    std::sort(order.begin(), order.end());

    std::vector<uint64_t> taken;

    for (const auto &entry : order)
    {
      auto &p = L.works.at(std::get<5>(entry));
      const auto &intent = p.intent;

      if (intent.site && L.sites.count({intent.settlement, *intent.site}))
      {
        close(p, "site taken");
        taken.push_back(p.id);

        continue;
      }

      // A household buys the timber and stone it lacks from the store first,
      // if the store has them and the purse can pay for both.
      if (intent.sponsor == CitySponsorKind::Household)
      {
        const auto purse = economy.accounts().find(intent.household);
        CityStock missing;
        bool any = false;

        for (uint32_t g = 0; g < cityGoodCount; ++g)
        {
          if (CityGood(g) == CityGood::Coin)
          {
            continue;
          }

          const int64_t have = purse == economy.accounts().end() ? 0 : purse->second.stock.units[g];

          if (p.cost.units[g] > have)
          {
            missing.units[g] = p.cost.units[g] - have;
            any = true;
          }
        }

        if (any)
        {
          int64_t price = 0;

          for (uint32_t g = 0; g < cityGoodCount; ++g)
          {
            price += missing.units[g] * economy.policy().prices[g];
          }

          const int64_t coin = purse == economy.accounts().end() ? 0 : purse->second.stock[CityGood::Coin];

          if (coin >= price + p.cost[CityGood::Coin])
          {
            economy.buyFromStore(intent.household, intent.settlement, missing, week);
          }
        }
      }

      const bool paid = intent.sponsor == CitySponsorKind::Treasury ? economy.spendTreasury(intent.settlement, p.cost) : economy.spendHousehold(intent.household, p.cost);

      if (paid)
      {
        p.escrow = p.cost;
        p.state = CityProjectState::Funded;
        p.fundedWeek = week;
        p.blockedBy.clear();

        if (intent.site)
        {
          L.sites.insert({intent.settlement, *intent.site});
        }

        add(CityRealmEventKind::ProjectFunded, {}, intent.settlement, intent.sponsorPerson, {}, {}, p.id, p.cost[CityGood::Coin], intent.reason);

        continue;
      }

      CityStock have;

      if (intent.sponsor == CitySponsorKind::Treasury)
      {
        const auto town = economy.settlements().find(intent.settlement);
        const auto seat = economy.settlements().find(economy.treasuryOf(intent.settlement));

        if (town != economy.settlements().end())
        {
          have = town->second.store;
          have[CityGood::Coin] = seat == economy.settlements().end() ? 0 : seat->second.treasury;
        }
      }
      else if (const auto purse = economy.accounts().find(intent.household); purse != economy.accounts().end())
      {
        have = purse->second.stock;
      }

      const std::string why = "unaffordable: needs " + describe(p.cost) + ", has " + describe(have);

      if (p.blockedBy != why)
      {
        p.blockedBy = why;
        add(CityRealmEventKind::ProjectBlocked, {}, intent.settlement, intent.sponsorPerson, {}, {}, p.id, 0, why);
      }
    }

    for (auto id : taken)
    {
      L.close(id);
    }
  }

  void review()
  {
    householdRules();
    treasuryRules();
    resolve();
  }

  void remember()
  {
    L.living.clear();

    for (const auto &[id, p] : snapshot.people)
    {
      if (p.alive)
      {
        L.living.insert(id);
      }
    }

    L.lastMembers.clear();

    for (const auto &[id, h] : homes.households())
    {
      if (!h.dissolvedWeek)
      {
        L.lastMembers[id] = h.members;
      }
    }
  }
};

bool CityRealmLedger::observe(const CityHistorySnapshot &snapshot, const CityHouseholdLedger &homes, CityEconomyLedger &economy, std::string *error)
{
  if (lastWeek && uint64_t(snapshot.week) != uint64_t(*lastWeek) + 1)
  {
    return fail(error, "realm observations must be consecutive weeks");
  }

  if (homes.week() != std::optional<uint32_t>(snapshot.week) || economy.week() != std::optional<uint32_t>(snapshot.week))
  {
    return fail(error, "the realm follows the household and economy ledgers of the same week");
  }

  // Both ledgers roll back together if the week does not validate.
  const size_t mark = log->size(), economyMark = economy.eventMark();
  CityRealmLedger next = *this;
  CityEconomyLedger accounts = economy;

  if (!next.firstWeek)
  {
    next.firstWeek = snapshot.week;
  }

  next.lastWeek = snapshot.week;

  Week week(next, snapshot, homes, accounts);

  week.run();

  if (!next.validate(snapshot, error) || !accounts.validate(snapshot, homes, error))
  {
    log->resize(mark);
    economy.truncateEvents(economyMark);

    return false;
  }

  *this = std::move(next);
  economy = std::move(accounts);

  if (error)
  {
    error->clear();
  }

  return true;
}

bool CityRealmLedger::validate(const CityHistorySnapshot &snapshot, std::string *error) const
{
  if (lastWeek != std::optional<uint32_t>(snapshot.week))
  {
    return fail(error, "realm validated against a different week");
  }

  auto alive = [&](std::optional<HistoryNode> id) {
    if (!id)
    {
      return true;
    }

    const auto it = snapshot.people.find(*id);

    return it != snapshot.people.end() && it->second.alive;
  };

  for (const auto &[realm, c] : crowns)
  {
    if (!alive(c.ruler) || !alive(c.consort) || !alive(c.heir) || !alive(c.regent))
    {
      return fail(error, "a dead person holds a live office");
    }

    for (auto id : c.claimants)
    {
      if (!alive(id))
      {
        return fail(error, "a dead claimant keeps a claim");
      }
    }

    if (c.regent && c.regent == c.ruler)
    {
      return fail(error, "the ruler is their own regent");
    }

    if ((c.authority == CityAuthorityKind::Ruler) != (c.ruler && !c.regent && (snapshot.week - snapshot.people.at(*c.ruler).bornWeek) / 52 >= succession.majorityAge))
    {
      return fail(error, "authority disagrees with the crown");
    }

    if ((c.authority == CityAuthorityKind::Regent) != c.regent.has_value())
    {
      return fail(error, "a regency without a regent, or a regent without a regency");
    }
  }

  std::set<std::pair<HistoryNode, std::string>> reserved;

  for (const auto &[site, count] : *completedSites)
  {
    if (count > 1)
    {
      return fail(error, "two completed projects share a site");
    }
    if (count)
    {
      reserved.insert(site);
    }
  }

  for (const auto &[id, p] : works)
  {
    const bool holds = p.state == CityProjectState::Funded || p.state == CityProjectState::Building || p.state == CityProjectState::Suspended;

    if (p.state == CityProjectState::Funded && !(p.escrow == p.cost))
    {
      return fail(error, "a funded project's escrow does not match its cost");
    }

    if (holds || p.state == CityProjectState::Completed)
    {
      CityStock held = p.escrow;

      held += p.spent;

      if (!(held == p.cost) || !p.cost.covers(p.spent))
      {
        return fail(error, "a project's escrow and spending do not add up to its cost");
      }

      if (p.intent.site && !reserved.insert({p.intent.settlement, *p.intent.site}).second)
      {
        return fail(error, "two live projects share a site");
      }
    }
    else if (!(p.escrow == CityStock{}))
    {
      return fail(error, "an unfunded project holds an escrow");
    }
  }

  if (reserved != sites)
  {
    std::string detail;

    for (const auto &[settlement, site] : sites)
    {
      detail += reserved.count({settlement, site}) ? "" : " register-only:" + site;
    }

    for (const auto &[settlement, site] : reserved)
    {
      detail += sites.count({settlement, site}) ? "" : " project-only:" + site;
    }

    return fail(error, "the site register disagrees with the funded projects:" + detail);
  }

  if (error)
  {
    error->clear();
  }

  return true;
}

double CityRealmLedger::safety(HistoryNode settlement, const CityHistorySnapshot &snapshot, const CityEconomyLedger &economy, const CityHouseholdLedger &homes) const
{
  std::set<HistoryNode> houses, feuding;

  for (const auto &[id, p] : snapshot.people)
  {
    if (p.alive && p.settlement == settlement && p.dynasty)
    {
      houses.insert(*p.dynasty);
    }
  }

  for (const auto &[a, b] : snapshot.feuds)
  {
    if (houses.count(a))
    {
      feuding.insert(a);
    }

    if (houses.count(b))
    {
      feuding.insert(b);
    }
  }

  double value = houses.empty() ? 1.0 : 1.0 - double(feuding.size()) / double(houses.size());

  value += 0.1 * economy.facts(settlement, homes).guards;

  for (const auto &[realm, c] : crowns)
  {
    if (c.seat == settlement && c.authority == CityAuthorityKind::None)
    {
      value -= 0.2;
    }
  }

  return clamp01(value);
}

void CityRealmLedger::close(uint64_t id)
{
  const auto it = works.find(id);

  if (it == works.end())
  {
    return;
  }

  // The archive is shared by transactional ledger copies. Replacing an
  // existing ID must replace its index contribution as well.
  if (const auto previous = archive->find(id); previous != archive->end())
  {
    const auto &old = previous->second;
    if (old.state == CityProjectState::Completed || old.state == CityProjectState::Retired)
    {
      auto count = completedCounts->find({old.intent.kind, old.intent.settlement});
      if (count != completedCounts->end() && count->second && --count->second == 0)
      {
        completedCounts->erase(count);
      }
    }
    if (old.state == CityProjectState::Completed && old.intent.site)
    {
      auto count = completedSites->find({old.intent.settlement, *old.intent.site});
      if (count != completedSites->end() && count->second && --count->second == 0)
      {
        completedSites->erase(count);
      }
    }
  }
  const auto &p = it->second;
  if (p.state == CityProjectState::Completed || p.state == CityProjectState::Retired)
  {
    ++(*completedCounts)[{p.intent.kind, p.intent.settlement}];
  }
  if (p.state == CityProjectState::Completed && p.intent.site)
  {
    ++(*completedSites)[{p.intent.settlement, *p.intent.site}];
  }
  (*archive)[id] = p;
  works.erase(it);
}

void CityRealmLedger::refund(CityProject &p, CityEconomyLedger &economy)
{
  if (p.intent.sponsor == CitySponsorKind::Treasury)
  {
    economy.refundTreasury(p.intent.settlement, p.escrow);
  }
  else
  {
    economy.refundHousehold(p.intent.household, p.escrow);
  }

  p.escrow = CityStock{};

  if (p.intent.site && p.state != CityProjectState::Completed)
  {
    sites.erase({p.intent.settlement, *p.intent.site});
  }
}

bool CityRealmLedger::startBuilding(uint64_t id, uint32_t week, std::string *error)
{
  const auto it = works.find(id);

  if (it == works.end() || it->second.state != CityProjectState::Funded)
  {
    return fail(error, "only a funded project can start building");
  }

  it->second.state = CityProjectState::Building;
  it->second.startedWeek = week;
  it->second.blockedBy.clear();
  log->push_back({week, CityRealmEventKind::ProjectStarted, {}, it->second.intent.settlement, it->second.intent.sponsorPerson, {}, {}, id, 0, it->second.intent.reason});

  return true;
}

bool CityRealmLedger::suspendProject(uint64_t id, uint32_t week, const std::string &reason, std::string *error)
{
  const auto it = works.find(id);

  if (it == works.end() || it->second.state != CityProjectState::Building)
  {
    return fail(error, "only a project under construction can be suspended");
  }

  it->second.state = CityProjectState::Suspended;
  it->second.blockedBy = reason;
  log->push_back({week, CityRealmEventKind::ProjectSuspended, {}, it->second.intent.settlement, it->second.intent.sponsorPerson, {}, {}, id, 0, reason});

  return true;
}

bool CityRealmLedger::resumeProject(uint64_t id, uint32_t week, std::string *error)
{
  const auto it = works.find(id);

  if (it == works.end() || it->second.state != CityProjectState::Suspended)
  {
    return fail(error, "only a suspended project can resume");
  }

  it->second.state = CityProjectState::Building;
  it->second.blockedBy.clear();
  log->push_back({week, CityRealmEventKind::ProjectResumed, {}, it->second.intent.settlement, it->second.intent.sponsorPerson, {}, {}, id, 0, ""});

  return true;
}

bool CityRealmLedger::drawEscrow(uint64_t id, const CityStock &part, std::string *error)
{
  const auto it = works.find(id);

  if (it == works.end() || (it->second.state != CityProjectState::Building && it->second.state != CityProjectState::Suspended))
  {
    return fail(error, "only a project under construction draws its escrow");
  }

  if (!it->second.escrow.covers(part))
  {
    return fail(error, "the escrow does not cover the draw");
  }

  it->second.escrow -= part;
  it->second.spent += part;

  return true;
}

bool CityRealmLedger::completeProject(uint64_t id, uint32_t week, CityEconomyLedger &economy, std::string *error)
{
  const auto it = works.find(id);

  if (it == works.end() || it->second.state != CityProjectState::Building)
  {
    return fail(error, "only a project under construction can complete");
  }

  auto &p = it->second;

  // Whatever the builders did not need goes home; the cost stays booked and
  // the site stays taken by what now stands on it.
  const CityStock leftover = p.escrow;

  p.state = CityProjectState::Completed;
  refund(p, economy);
  p.cost -= leftover;
  p.completedWeek = week;
  log->push_back({week, CityRealmEventKind::ProjectCompleted, {}, p.intent.settlement, p.intent.sponsorPerson, {}, {}, id, leftover[CityGood::Coin], p.intent.reason});
  close(id);

  return true;
}

bool CityRealmLedger::rejectProject(uint64_t id, uint32_t week, const std::string &reason, CityEconomyLedger &economy, std::string *error)
{
  const auto it = works.find(id);
  const auto state = it == works.end() ? CityProjectState::Cancelled : it->second.state;

  if (state != CityProjectState::Funded && state != CityProjectState::Building && state != CityProjectState::Suspended && state != CityProjectState::Proposed)
  {
    return fail(error, "only an open project can be rejected");
  }

  auto &p = it->second;

  if (state != CityProjectState::Proposed)
  {
    refund(p, economy);
  }

  p.state = CityProjectState::Cancelled;
  p.closedWeek = week;
  p.blockedBy = reason;
  log->push_back({week, CityRealmEventKind::ProjectCancelled, {}, p.intent.settlement, p.intent.sponsorPerson, {}, {}, id, 0, reason});
  close(id);

  return true;
}

bool CityRealmLedger::retireProject(uint64_t id, uint32_t week, const std::string &reason, std::string *error)
{
  const auto it = archive->find(id);

  if (it == archive->end() || it->second.state != CityProjectState::Completed)
  {
    return fail(error, "only a completed project can be retired");
  }

  if (it->second.intent.site)
  {
    const auto key = std::make_pair(it->second.intent.settlement, *it->second.intent.site);
    auto count = completedSites->find(key);
    if (count != completedSites->end() && --count->second == 0)
    {
      completedSites->erase(count);
    }
  }
  it->second.state = CityProjectState::Retired;
  it->second.closedWeek = week;
  it->second.blockedBy = reason;

  if (it->second.intent.site)
  {
    sites.erase({it->second.intent.settlement, *it->second.intent.site});
  }

  log->push_back({week, CityRealmEventKind::ProjectRetired, {}, it->second.intent.settlement, it->second.intent.sponsorPerson, {}, {}, id, 0, reason});

  return true;
}

void CityRealmLedger::noteProject(uint64_t id, const std::string &note)
{
  const auto it = works.find(id);

  if (it != works.end() && it->second.blockedBy != note)
  {
    it->second.blockedBy = note;
  }
}

const char *cityRealmEventName(CityRealmEventKind k)
{
  static const char *const names[] = {"crowned",       "crown-released", "heir-named",   "heir-released",    "consort-taken",  "consort-released", "claim-raised",    "claim-dropped",   "regency-begun",
                                      "regency-ended", "no-authority",   "inherited",    "escheated",        "project-proposed", "project-funded", "project-blocked", "project-cancelled",
                                      "project-started", "project-suspended", "project-resumed", "project-completed", "project-retired"};

  return uint32_t(k) < 22 ? names[uint32_t(k)] : "unknown";
}

const char *cityAuthorityName(CityAuthorityKind k)
{
  return k == CityAuthorityKind::Ruler ? "ruler" : k == CityAuthorityKind::Regent ? "regent" : "none";
}

const char *cityProjectKindName(CityProjectKind k)
{
  static const char *const names[] = {"home", "shop", "inn", "workshop", "farm", "market", "church", "keep", "wall", "road", "stair", "repair", "extension", "rebuild", "manor", "palisade"};

  return uint32_t(k) < cityProjectKindCount ? names[uint32_t(k)] : "unknown";
}

const char *cityProjectStateName(CityProjectState s)
{
  static const char *const names[] = {"proposed", "reserved", "funded", "building", "completed", "suspended", "cancelled", "retired"};

  return uint32_t(s) < 8 ? names[uint32_t(s)] : "unknown";
}
} // namespace procedural::world::rewrite
