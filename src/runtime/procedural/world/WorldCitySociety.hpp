#pragma once

// Phase 9 of the living city history: a society the ledgers can be scaled
// against.
//
// The story matcher is the reference for the social history and it is the
// cost that grows with the cast: every courtship rule runs once per pair of
// grown villagers every week. To measure the city ledgers at hundreds and
// thousands of people, and to balance them over centuries, this driver
// produces the same weekly snapshot the story does, from a small authored
// demographic model instead of the rules. It is a scaling and balancing
// fixture, and it says so: it is not the game's social simulation, and every
// person it makes is still a named identity with parents, a house, a spouse
// and a settlement, so the ledgers see exactly what the story gives them.

#include "WorldCityMigration.hpp"
#include <random>

namespace procedural::world::rewrite {

struct CitySocietySettings
{
  uint32_t seed = 1;
  uint32_t startWeek = 1000 * 52;
  std::vector<std::pair<HistoryNode, uint32_t>> settlements; // Settlement node, founding households.
  std::map<HistoryNode, uint32_t> foundingPopulations; // Exact census, when supplied.
  HistoryNode realm = 900;                                    // The first settlement's first house holds the crown.
  HistoryNode originSettlement = 950;                         // Where newcomers come from.
  double birthsPerFertileCoupleYear = .18;                    // Married, woman 18-44.
  double marriagesPerSingleAdultYear = .12;                   // 18+, unmarried, within a settlement.
  double infantDeathsPerYear = .03, adultDeathsPerYear = .008, elderDeathsPerYear = .09;
  double emigrantHouseholdsPerYear = 0;                       // Whole households leaving for the origin.
  double immigrantHouseholdsPerYearAtFullPull = 3;            // Scaled by the pull each settlement reports.
  uint32_t waitingTolerance = 2;                              // Newcomers still come while fewer than this many wait for a home.
  uint32_t maximumFeudYears = 1; // Seeded duration, one to this many years.
  double feudsPerYear = .05;                                  // Between two houses of one settlement.
  uint32_t maximumLiving = 5000;                              // A guard, never a death.
};

// One settlement's pull on newcomers, from the ledgers of the week before.
struct CitySocietyPull
{
  double work = 0, food = 0, safety = 0;
  uint32_t vacantHomes = 0, waitingHouseholds = 0;
};

class CitySocietyDriver
{
public:
  explicit CitySocietyDriver(CitySocietySettings);

  // The snapshot for the next week. Pulls are read per settlement; a
  // settlement with none has no immigration.
  const CityHistorySnapshot &step(const std::map<HistoryNode, CitySocietyPull> &pulls = {});

  const CityHistorySnapshot &current() const
  {
    return snapshot;
  }

  uint32_t living() const;
  uint32_t week() const
  {
    return snapshot.week;
  }

  struct Counts
  {
    uint32_t births = 0, deaths = 0, marriages = 0, arrivals = 0, departures = 0, successions = 0, feuds = 0;
  };

  const Counts &counts() const
  {
    return tally;
  }

private:
  struct Person
  {
    bool man = true;
    uint32_t house = 0;
  };

  CitySocietySettings settings;
  CityHistorySnapshot snapshot;
  std::map<HistoryNode, Person> extra; // Gender and house, which the snapshot keeps as dynasty.
  std::mt19937_64 rng;
  HistoryNode nextNode = 0;
  std::map<uint64_t, uint32_t> feudSince; // House pair key -> week.
  Counts tally;

  bool chance(double perYear);
  uint32_t ageOf(HistoryNode) const;
  HistoryNode makePerson(bool man, uint32_t bornWeek, HistoryNode settlement, HistoryNode house, std::vector<HistoryNode> parents);
  void founding();
  void ageing();
  void deaths();
  void marriages();
  void births();
  void migration(const std::map<HistoryNode, CitySocietyPull> &pulls);
  void feuds();
  void crown();
  void restage();
};

} // namespace procedural::world::rewrite
