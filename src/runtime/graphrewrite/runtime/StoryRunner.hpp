#pragma once

// Runs a rule programme over a starting world, tick after tick, and keeps the
// record a narrator needs: every rule that fired, on whom, when.
//
// One tick is: match every rule against the graph as it stands, shuffle the
// matched rows (deterministically, from the seed), apply what the dice allow,
// and note what fired. Matching runs on the CPU runner - the cast of a story is
// dozens of people, not a million edges, and the CPU path is the one whose every
// firing is observable.
//
// The tick also computes GLOBAL CONTEXT the rules cannot see - how many people
// are alive - and feeds it back as a chance multiplier on the death rule. A rule
// file cannot say "more deaths when the village is crowded" because a rule only
// sees the edges it matched; this is the one place the whole graph gets a vote.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/compiler/UsageValues.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"
#include "graphrewrite/runtime/ExpressionHost.hpp"
#include "graphrewrite/runtime/FactsFile.hpp"
#include "graphrewrite/runtime/NameGenerator.hpp"
#include "graphrewrite/runtime/Narrator.hpp"
#include "graphrewrite/runtime/RewriteApplier.hpp"

#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <random>
#include <vector>

namespace graphrewrite::runtime {

struct StorySettings
{
  uint32_t seed = 1u;

  // One tick is one WEEK. `ticks` is how many the story runs for; `startWeek`
  // is the absolute week the clock starts at, so a birth year in the facts
  // (`Aldric born in 965`) is a week node the age guards can measure against.
  // Year 1000, week 0, is the first week of the story.
  uint32_t ticks = 52u;
  uint32_t startWeek = 1000u * 52u;

  // Above this many living people the death rule's chance rises with the
  // excess; below it the rule fires at a tenth of its declared chance. Zero
  // leaves the rule's own chance alone.
  uint32_t populationCap = 24u;

  std::string deathRule = "death-comes";

  // The edge types the driver reads for context and gender. Patterns, as the
  // rule file declares them.
  std::string aliveType = "? is alive";

  // Week nodes: `week(x)` in a clause names the node dayBase + x, so a date can
  // be an edge in the graph and a later rule can read it back with weekOf().
  // (`day*` spellings are kept as aliases.)
  uint32_t dayBase = 1u << 24u;
  std::string manType = "? is man";

  // Read to give someone born during the run their family's name.
  std::string houseType = "? is of house ?";
  std::string womanType = "? is woman";

  uint32_t rowCapacityPerTable = 8192u;

  // Keep every firing for the narrator. Off for a run that only wants the
  // footer: a world of thousands fires millions of times.
  bool recordEvents = true;

  // Group the story by month instead of by week.
  bool byMonth = false;
};

struct TickSummary
{
  uint32_t tick = 0u;
  uint32_t population = 0u;
  uint32_t matchedRows = 0u;
  uint32_t fired = 0u;
  uint32_t scheduled = 0u;
  uint32_t cancelled = 0u;
  uint32_t released = 0u;
  uint32_t guarded = 0u;
  uint32_t declined = 0u;
  uint32_t stale = 0u;
  uint32_t rejected = 0u;
  uint32_t born = 0u;

  // The matcher ran out of table rows this week: some matches were never
  // seen. Raise StorySettings::rowCapacityPerTable.
  bool overflowed = false;

  // Wall time of this simulation tick, and where it went: the match (the
  // programme's instruction loop), gathering rows and rolling dice, the
  // rewrite. `programTicks` is how many programme instructions the match ran.
  double seconds = 0.0;
  double matchSeconds = 0.0;
  double gatherSeconds = 0.0;
  double applySeconds = 0.0;
  uint32_t programTicks = 0u;
};

class StoryRunner
{
public:
  StoryRunner(const compiler::CompiledProgram &program, const compiler::UsageValues &usage, const LoadedFacts &world,
              const Narrator &narrator, StorySettings settings);

  // One tick. Returns the number of rows that fired.
  uint32_t step();

  // Every remaining tick, stopping early after `quietTicks` ticks with no
  // firing at all - a village that has gone completely still has no more story.
  void run(uint32_t quietTicks = 8u);

  // The whole story so far, narrated and formatted. Deterministic per seed.
  std::string text();

  // The closing block: the living, the weeks, and what the run cost in wall
  // time. Separate from text() because the clock is not reproducible.
  std::string footer() const;

  // "early April" / "late November of the third year": how a week reads in
  // the text. Relative to the story's first week.
  static std::string weekPhrase(uint32_t week, uint32_t startWeek);

  // One narrated week, formatted the way text() does it. With `byMonth`, the
  // header is the month and is printed only when it changes from
  // `lastMonth` (an in/out key: year * 12 + month, ~0u for none yet).
  static void appendNarratedTick(std::ostream &out, const NarratedTick &tick, uint32_t startWeek, bool byMonth = false,
                                 uint32_t *lastMonth = nullptr);

  // "April" / "April of the third year", and the key above, for a week.
  static std::string monthPhrase(uint32_t week, uint32_t startWeek, uint32_t &key);

  // The week relative to the story's start, as the headers count it.
  uint32_t storyWeek() const
  {
    return currentTick - settings.startWeek;
  }

  // Wall-clock seconds spent in step() so far.
  double elapsedSeconds() const
  {
    return simulatedSeconds;
  }

  // Weeks in which the matcher overflowed a table and matches were lost.
  uint32_t overflowedWeeks() const
  {
    return overflowedTicks;
  }

  const std::vector<FiredEvent> &events() const
  {
    return log;
  }

  const std::vector<TickSummary> &summaries() const
  {
    return ticks;
  }

  NameTable &names()
  {
    return nameTable;
  }

  // The clause handlers. `nowInDays()` and `availableAt(place, day)` are
  // registered by the constructor; a programme may add its own before run().
  ExpressionHost &expressions()
  {
    return host;
  }

  // The church booking table `availableAt` keeps: (place, day) pairs taken.
  const std::set<std::pair<uint32_t, uint32_t>> &bookings() const
  {
    return booked;
  }

  uint32_t population() const;
  Gender genderOf(uint32_t id) const;

  // The node of this person's house, or ~0u.
  uint32_t familyOf(uint32_t id) const;

  const EdgePageStore &graph() const
  {
    return store;
  }

  // Facts published from OUTSIDE the rules between ticks, by an adapter that
  // owns a fact domain the rules do not (the city's migration). It shares the
  // rewrite's vertex allocator, so a node it makes is a node like any other.
  // Write between step() calls only: the matcher reads the store as it stands.
  EdgePageStore &mutableGraph()
  {
    return store;
  }

private:
  uint32_t scaledChance(uint32_t rule, uint32_t permille) const;

  const compiler::CompiledProgram &program;
  const Narrator &narrator;
  StorySettings settings;

  EdgePageStore store;
  std::unique_ptr<CpuProgramRunner> runner;
  std::unique_ptr<RewriteApplier> applier;
  NameTable nameTable;
  ExpressionHost host;
  std::set<std::pair<uint32_t, uint32_t>> booked;

  compiler::EdgeTypeId alive = compiler::invalidEdgeType;
  compiler::EdgeTypeId man = compiler::invalidEdgeType;
  compiler::EdgeTypeId woman = compiler::invalidEdgeType;
  compiler::EdgeTypeId house = compiler::invalidEdgeType;
  std::vector<bool> isDeathRule;
  std::vector<FiredEvent> log;
  std::vector<TickSummary> ticks;
  uint32_t currentTick = 0u;
  std::mt19937 rolls;

  // Wall clock, for the footer: how long the whole run took and the slowest week.
  double simulatedSeconds = 0.0;
  double slowestTickSeconds = 0.0;
  double matchSeconds = 0.0;
  double gatherSeconds = 0.0;
  double applySeconds = 0.0;
  uint64_t programTicks = 0u;
  uint32_t overflowedTicks = 0u;
  uint32_t currentPopulation = 0u;
};

} // namespace graphrewrite::runtime
