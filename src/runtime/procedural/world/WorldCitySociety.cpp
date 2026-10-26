#include "WorldCitySociety.hpp"
#include <algorithm>

namespace procedural::world::rewrite {
namespace {
uint64_t pairKey(HistoryNode a, HistoryNode b)
{
  return (uint64_t(std::min(a, b)) << 32) | std::max(a, b);
}

CityLifeStage stageAt(uint32_t age)
{
  return age < 12 ? CityLifeStage::Child : age < 18 ? CityLifeStage::ApprenticeAge : age < 65 ? CityLifeStage::Adult : CityLifeStage::Elder;
}
} // namespace

CitySocietyDriver::CitySocietyDriver(CitySocietySettings s) : settings(std::move(s)), rng(uint64_t(settings.seed) * 0x9e3779b97f4a7c15ull + 11)
{
  snapshot.week = settings.startWeek;
  founding();
  restage();
  crown();
}

bool CitySocietyDriver::chance(double perYear)
{
  return std::uniform_real_distribution<double>(0, 1)(rng) < perYear / 52.0;
}

uint32_t CitySocietyDriver::ageOf(HistoryNode id) const
{
  return (snapshot.week - snapshot.people.at(id).bornWeek) / 52;
}

HistoryNode CitySocietyDriver::makePerson(bool man, uint32_t bornWeek, HistoryNode settlement, HistoryNode house, std::vector<HistoryNode> parents)
{
  const auto id = nextNode++;
  CityHistoryPerson p;

  p.id = id;
  p.bornWeek = bornWeek;
  p.alive = true;
  p.settlement = settlement;
  p.dynasty = house;
  p.parents = std::move(parents);
  std::sort(p.parents.begin(), p.parents.end());
  p.stage = stageAt((snapshot.week - bornWeek) / 52);
  snapshot.people[id] = p;
  extra[id] = {man, house};

  return id;
}

void CitySocietyDriver::founding()
{
  uint32_t house = 100000;
  bool first = true;

  for (const auto &[settlement, households] : settings.settlements)
  {
    const auto census = settings.foundingPopulations.find(settlement);
    const uint32_t families = census == settings.foundingPopulations.end() ? households : (census->second + 3) / 4;
    uint32_t remainingChildren = census == settings.foundingPopulations.end() ? 0 : census->second - 2 * families;
    for (uint32_t h = 0; h < families; ++h, ++house)
    {
      const uint32_t manAge = 24 + uint32_t(rng() % 18), womanAge = 20 + uint32_t(rng() % 16);
      const auto man = makePerson(true, snapshot.week - manAge * 52 - uint32_t(rng() % 52), settlement, house, {});
      const auto woman = makePerson(false, snapshot.week - womanAge * 52 - uint32_t(rng() % 52), settlement, house, {});

      snapshot.people[man].spouse = woman;
      snapshot.people[woman].spouse = man;
      snapshot.houseHeads[house] = man;
      snapshot.sworn[house] = settings.realm;

      const uint32_t children = census == settings.foundingPopulations.end() ? uint32_t(rng() % 4) :
        std::min(3u, (remainingChildren + families - h - 1) / (families - h));
      if (census != settings.foundingPopulations.end()) remainingChildren -= children;

      for (uint32_t c = 0; c < children; ++c)
      {
        const uint32_t age = uint32_t(rng() % std::max(1u, std::min(womanAge, manAge) - 17));

        makePerson(rng() % 2 == 0, snapshot.week - age * 52 - 1 - uint32_t(rng() % 51), settlement, house, {man, woman});
      }

      if (first)
      {
        snapshot.rulers[settings.realm] = man;
        snapshot.consorts[settings.realm] = woman;
        first = false;
      }
    }
  }
}

void CitySocietyDriver::restage()
{
  for (auto &[id, p] : snapshot.people)
  {
    if (p.alive)
    {
      p.stage = stageAt((snapshot.week - p.bornWeek) / 52);
    }
  }
}

void CitySocietyDriver::ageing()
{
  restage();
}

void CitySocietyDriver::deaths()
{
  for (auto &[id, p] : snapshot.people)
  {
    if (!p.alive)
    {
      continue;
    }

    const uint32_t age = (snapshot.week - p.bornWeek) / 52;
    const double rate = age < 5 ? settings.infantDeathsPerYear : age < 65 ? settings.adultDeathsPerYear : settings.elderDeathsPerYear * (1 + (age - 65) / 10.0);

    if (chance(rate))
    {
      p.alive = false;
      ++tally.deaths;

      if (p.spouse)
      {
        snapshot.people.at(*p.spouse).spouse.reset();
        p.spouse.reset();
      }
    }
  }

  // Offices and headships pass from the dead.
  for (auto it = snapshot.houseHeads.begin(); it != snapshot.houseHeads.end();)
  {
    if (!snapshot.people.at(it->second).alive)
    {
      std::optional<HistoryNode> heir;

      for (const auto &[id, p] : snapshot.people)
      {
        if (p.alive && p.dynasty == it->first && p.stage == CityLifeStage::Adult && extra.at(id).man && (!heir || p.bornWeek < snapshot.people.at(*heir).bornWeek))
        {
          heir = id;
        }
      }

      if (heir)
      {
        it->second = *heir;
        ++it;
      }
      else
      {
        it = snapshot.houseHeads.erase(it);
      }
    }
    else
    {
      ++it;
    }
  }

  for (auto table : {&snapshot.rulers, &snapshot.consorts, &snapshot.heirs})
  {
    for (auto it = table->begin(); it != table->end();)
    {
      it = snapshot.people.at(it->second).alive ? std::next(it) : table->erase(it);
    }
  }
}

void CitySocietyDriver::marriages()
{
  std::map<HistoryNode, std::vector<HistoryNode>> men, women;

  for (const auto &[id, p] : snapshot.people)
  {
    if (p.alive && p.settlement && !p.spouse && p.stage == CityLifeStage::Adult)
    {
      (extra.at(id).man ? men : women)[*p.settlement].push_back(id);
    }
  }

  for (auto &[settlement, grooms] : men)
  {
    auto &brides = women[settlement];

    for (auto groom : grooms)
    {
      if (brides.empty() || !chance(settings.marriagesPerSingleAdultYear))
      {
        continue;
      }

      // Not a sibling, not a parent: the story forbids both.
      const auto &g = snapshot.people.at(groom);
      std::optional<size_t> pick;

      for (size_t i = 0; i < brides.size(); ++i)
      {
        const auto &b = snapshot.people.at(brides[i]);
        std::vector<HistoryNode> shared;

        std::set_intersection(g.parents.begin(), g.parents.end(), b.parents.begin(), b.parents.end(), std::back_inserter(shared));

        if (shared.empty() && !std::binary_search(g.parents.begin(), g.parents.end(), brides[i]) && !std::binary_search(b.parents.begin(), b.parents.end(), groom))
        {
          pick = i;

          break;
        }
      }

      if (!pick)
      {
        continue;
      }

      const auto bride = brides[*pick];

      brides.erase(brides.begin() + long(*pick));
      snapshot.people.at(groom).spouse = bride;
      snapshot.people.at(bride).spouse = groom;
      // The bride joins her husband's house, as the story has it.
      snapshot.people.at(bride).dynasty = g.dynasty;
      extra.at(bride).house = extra.at(groom).house;
      ++tally.marriages;
    }
  }
}

void CitySocietyDriver::births()
{
  if (living() >= settings.maximumLiving)
  {
    return;
  }

  std::vector<std::pair<HistoryNode, HistoryNode>> couples;

  for (const auto &[id, p] : snapshot.people)
  {
    if (p.alive && p.spouse && !extra.at(id).man && p.settlement)
    {
      const uint32_t age = (snapshot.week - p.bornWeek) / 52;

      if (age >= 18 && age < 45 && snapshot.people.at(*p.spouse).alive)
      {
        couples.push_back({*p.spouse, id});
      }
    }
  }

  for (const auto &[father, mother] : couples)
  {
    if (chance(settings.birthsPerFertileCoupleYear))
    {
      const auto &m = snapshot.people.at(mother);

      makePerson(rng() % 2 == 0, snapshot.week, *m.settlement, *snapshot.people.at(father).dynasty, {father, mother});
      ++tally.births;
    }
  }
}

void CitySocietyDriver::migration(const std::map<HistoryNode, CitySocietyPull> &pulls)
{
  // Departures: a whole household is a couple and their children in one house at one settlement.
  if (settings.emigrantHouseholdsPerYear > 0 && chance(settings.emigrantHouseholdsPerYear))
  {
    for (auto &[house, head] : snapshot.houseHeads)
    {
      const auto &h = snapshot.people.at(head);

      if (!h.alive || h.settlement == settings.originSettlement || snapshot.rulers.count(settings.realm) && snapshot.rulers.at(settings.realm) == head)
      {
        continue;
      }

      for (auto &[id, p] : snapshot.people)
      {
        if (p.alive && p.dynasty == house && p.settlement == h.settlement)
        {
          p.settlement = settings.originSettlement;
          ++tally.departures;
        }
      }

      break;
    }
  }

  if (living() >= settings.maximumLiving)
  {
    return;
  }

  for (const auto &[settlement, pull] : pulls)
  {
    const double attraction = std::min(1.0, std::max(0.0, .4 * pull.work + .4 * pull.food + .2 * pull.safety));

    if ((pull.vacantHomes == 0 && pull.waitingHouseholds >= settings.waitingTolerance) || !chance(settings.immigrantHouseholdsPerYearAtFullPull * attraction))
    {
      continue;
    }

    const uint32_t house = 200000 + uint32_t(extra.size());
    const uint32_t manAge = 20 + uint32_t(rng() % 20), womanAge = 18 + uint32_t(rng() % 18);
    const auto man = makePerson(true, snapshot.week - manAge * 52 - 1 - uint32_t(rng() % 51), settlement, house, {});
    const auto woman = makePerson(false, snapshot.week - womanAge * 52 - 1 - uint32_t(rng() % 51), settlement, house, {});

    snapshot.people[man].spouse = woman;
    snapshot.people[woman].spouse = man;
    snapshot.houseHeads[house] = man;
    snapshot.sworn[house] = settings.realm;

    const uint32_t children = uint32_t(rng() % 3);

    for (uint32_t c = 0; c < children; ++c)
    {
      const uint32_t age = uint32_t(rng() % std::max(1u, std::min(womanAge, manAge) - 17));

      makePerson(rng() % 2 == 0, snapshot.week - age * 52 - 1 - uint32_t(rng() % 51), settlement, house, {man, woman});
    }

    tally.arrivals += 2 + children;
  }
}

void CitySocietyDriver::feuds()
{
  for (auto it = feudSince.begin(); it != feudSince.end();)
  {
    if (snapshot.week - it->second >= 52 * (1 + ((it->first ^ settings.seed) % std::max(1u,settings.maximumFeudYears))))
    {
      const HistoryNode a = HistoryNode(it->first >> 32), b = HistoryNode(it->first & 0xffffffffu);

      snapshot.feuds.erase({a, b});
      snapshot.feuds.erase({b, a});
      it = feudSince.erase(it);
    }
    else
    {
      ++it;
    }
  }

  if (!chance(settings.feudsPerYear))
  {
    return;
  }

  std::map<HistoryNode, std::vector<HistoryNode>> houses;

  for (const auto &[house, head] : snapshot.houseHeads)
  {
    const auto &h = snapshot.people.at(head);

    if (h.alive && h.settlement)
    {
      houses[*h.settlement].push_back(house);
    }
  }

  for (const auto &[settlement, list] : houses)
  {
    if (list.size() < 2)
    {
      continue;
    }

    const auto a = list[rng() % list.size()], b = list[rng() % list.size()];

    if (a != b && !feudSince.count(pairKey(a, b)))
    {
      snapshot.feuds.insert({a, b});
      snapshot.feuds.insert({b, a});
      feudSince[pairKey(a, b)] = snapshot.week;
      ++tally.feuds;
    }

    break;
  }
}

void CitySocietyDriver::crown()
{
  const auto realm = settings.realm;

  if (snapshot.rulers.count(realm))
  {
    const auto ruler = snapshot.rulers.at(realm);

    if (!snapshot.heirs.count(realm))
    {
      std::optional<HistoryNode> heir;

      for (const auto &[id, p] : snapshot.people)
      {
        if (p.alive && std::binary_search(p.parents.begin(), p.parents.end(), ruler) && extra.at(id).man && (!heir || p.bornWeek < snapshot.people.at(*heir).bornWeek))
        {
          heir = id;
        }
      }

      if (heir)
      {
        snapshot.heirs[realm] = *heir;
      }
    }

    snapshot.vacantRealms.erase(realm);

    return;
  }

  // The crown is vacant: the heir takes it the week after, as the story's rule does.
  if (snapshot.vacantRealms.count(realm))
  {
    if (snapshot.heirs.count(realm) && snapshot.people.at(snapshot.heirs.at(realm)).alive)
    {
      const auto heir = snapshot.heirs.at(realm);

      snapshot.rulers[realm] = heir;
      snapshot.heirs.erase(realm);
      snapshot.vacantRealms.erase(realm);

      if (snapshot.people.at(heir).spouse)
      {
        snapshot.consorts[realm] = *snapshot.people.at(heir).spouse;
      }

      ++tally.successions;
    }
    else
    {
      // A sworn house's living head takes it when the line has ended.
      for (const auto &[house, head] : snapshot.houseHeads)
      {
        if (snapshot.people.at(head).alive && snapshot.people.at(head).stage == CityLifeStage::Adult)
        {
          snapshot.rulers[realm] = head;
          snapshot.vacantRealms.erase(realm);
          ++tally.successions;

          break;
        }
      }
    }
  }
  else
  {
    snapshot.vacantRealms.insert(realm);
  }
}

const CityHistorySnapshot &CitySocietyDriver::step(const std::map<HistoryNode, CitySocietyPull> &pulls)
{
  ++snapshot.week;
  ageing();
  deaths();
  marriages();
  births();
  migration(pulls);
  feuds();
  crown();

  return snapshot;
}

uint32_t CitySocietyDriver::living() const
{
  uint32_t n = 0;

  for (const auto &[id, p] : snapshot.people)
  {
    n += p.alive;
  }

  return n;
}
} // namespace procedural::world::rewrite
