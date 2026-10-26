#include "graphrewrite/runtime/StoryRunner.hpp"

#include "graphrewrite/runtime/Dice.hpp"
#include "graphrewrite/runtime/RuleRows.hpp"
#include "graphrewrite/runtime/StoryClauses.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <cstdint>
#include <random>
#include <sstream>

namespace graphrewrite::runtime {

StoryRunner::StoryRunner(const compiler::CompiledProgram &compiled, const compiler::UsageValues &usage, const LoadedFacts &world,
                         const Narrator &teller, StorySettings storySettings)
    : program(compiled), narrator(teller), settings(std::move(storySettings)), nameTable(settings.seed)
{
  store.configure(program.edgeTypes);

  program.edgeTypes.find(settings.aliveType, alive);
  program.edgeTypes.find(settings.manType, man);
  program.edgeTypes.find(settings.womanType, woman);
  program.edgeTypes.find(settings.houseType, house);

  CpuProgramRunner::Settings runnerSettings;

  runnerSettings.rowCapacityPerTable = settings.rowCapacityPerTable;

  runner = std::make_unique<CpuProgramRunner>(program, usage, store, runnerSettings);

  // The ceiling goes on BEFORE the facts are seeded: a birth week in the facts
  // is an edge to a node above dayBase, and adding it would otherwise push the
  // allocator past every week node, naming each newborn after a week.
  store.setVertexCeiling(settings.dayBase);
  runner->seed(world.facts);

  // Every vertex the world named is taken; newborns start above them - and
  // below the week nodes, which are values, not people.
  store.reserveVertices(static_cast<uint32_t>(world.names.size()));

  currentTick = settings.startWeek;
  rolls.seed(settings.seed * 0x9e3779b1u + 7u);

  for (uint32_t id = 0u; id < static_cast<uint32_t>(world.names.size()); ++id)
  {
    nameTable.setName(id, world.names[id]);
  }

  nameTable.setGenderLookup([this](const uint32_t id) { return genderOf(id); });
  nameTable.setFamilyLookup([this](const uint32_t id) { return familyOf(id); });

  applier = std::make_unique<RewriteApplier>(program, store, settings.seed);

  isDeathRule.assign(program.ruleNames.size(), false);

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    isDeathRule[rule] = ruleTitleOf(program.ruleNames[rule]) == settings.deathRule;
  }

  applier->setChanceScale([this](const uint32_t rule, const uint32_t permille) { return scaledChance(rule, permille); });

  applier->setFireObserver([this](const RewriteEvent &fired) {
    if (!settings.recordEvents)
    {
      return;
    }

    FiredEvent event;

    event.tick = currentTick;
    event.rule = fired.rule;
    event.bound = fired.bound;
    event.kind = fired.kind;

    log.push_back(std::move(event));
  });

  registerStoryClauses(host, settings.dayBase, booked, rolls);

  const uint32_t dayBase = settings.dayBase;

  const uint32_t startWeek = settings.startWeek;

  nameTable.setRangeNamer(dayBase, [dayBase, startWeek](const uint32_t id) { return weekPhrase(id - dayBase, startWeek); });

  applier->setHost(&host);

  currentPopulation = population();
}

uint32_t StoryRunner::population() const
{
  return alive == compiler::invalidEdgeType ? 0u : store.edgeCount(alive);
}

uint32_t StoryRunner::familyOf(const uint32_t id) const
{
  if (house == compiler::invalidEdgeType || !store.has(house, KeyOrder::ByOrigin))
  {
    return ~uint32_t(0);
  }

  const Partition &partition = store.partition(house, KeyOrder::ByOrigin);
  const PageRun run = partition.rangeFor(id);

  if (run.empty())
  {
    return ~uint32_t(0);
  }

  for (uint32_t position = run.first; position <= run.last && position < partition.pageCount(); ++position)
  {
    for (const Edge &edge : partition.pageAt(position))
    {
      if (edge.origin == id)
      {
        return edge.target;
      }
    }
  }

  return ~uint32_t(0);
}

Gender StoryRunner::genderOf(const uint32_t id) const
{
  if (man != compiler::invalidEdgeType && store.containsEdge(man, Edge{id, id}))
  {
    return Gender::Masculine;
  }

  if (woman != compiler::invalidEdgeType && store.containsEdge(woman, Edge{id, id}))
  {
    return Gender::Feminine;
  }

  return Gender::Unknown;
}

uint32_t StoryRunner::scaledChance(const uint32_t rule, const uint32_t permille) const
{
  if (rule >= isDeathRule.size() || !isDeathRule[rule] || settings.populationCap == 0u)
  {
    return permille;
  }

  // Below the cap, a tenth of the declared rate: `maybe 1` in the file becomes
  // one death in a hundred per tick, which is a quiet village. Above it, the
  // excess drives the rate up linearly until, at twice the cap, death is
  // certain for anyone the dice reach.
  const uint32_t quiet = permille / 10u;

  if (currentPopulation <= settings.populationCap)
  {
    return quiet;
  }

  const uint64_t excess = currentPopulation - settings.populationCap;
  const uint64_t pressure = excess * dice::permille / settings.populationCap;

  return static_cast<uint32_t>(std::min<uint64_t>(dice::permille, quiet + pressure));
}

uint32_t StoryRunner::step()
{
  const auto started = std::chrono::steady_clock::now();

  ++currentTick;

  TickSummary summary;

  summary.tick = currentTick;

  currentPopulation = population();

  // Today's date first: what was promised for today happens before anyone
  // matches against the graph, so the matcher sees the wedding that just took
  // place, and a suitor cannot plot against a marriage that already exists.
  applier->setNow(currentTick);

  const RewriteStats due = applier->applyDue();

  summary.released = due.released;
  summary.cancelled = due.cancelled;

  runner->reset();

  const auto matchStarted = std::chrono::steady_clock::now();

  // A false return is a table overflow (or starvation): the rows it did
  // produce are real, but some matches were never seen. Count it; the footer
  // and the driver say so, because a silent partial week reads as a quiet one.
  if (!runner->runToCompletion())
  {
    summary.overflowed = true;
    ++overflowedTicks;
  }

  summary.programTicks = runner->stats().instructionsCompleted;
  summary.matchSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - matchStarted).count();

  const auto gatherStarted = std::chrono::steady_clock::now();

  // The dice are rolled HERE, with exactly the inputs the applier rolls with
  // (same seed, same round, same rule, same table row), so a declined pair
  // never becomes a row object. The applier's own roll then agrees.
  const uint32_t roundSeed = dice::roundSeed(settings.seed, currentTick);

  std::vector<MatchedRow> rows;

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    const uint32_t chance = scaledChance(rule, program.ruleRewrites[rule].policy.permille());
    const uint32_t ruleSeed = dice::seedFor(roundSeed, rule);

    const RowFilter keep = chance >= dice::permille ? RowFilter()
                                                    : RowFilter([ruleSeed, chance](const uint32_t row) {
                                                        return dice::fires(ruleSeed, row, chance);
                                                      });

    std::vector<MatchedRow> matched = gatherRuleRows(program, *runner, rule, keep);

    rows.insert(rows.end(), std::make_move_iterator(matched.begin()), std::make_move_iterator(matched.end()));
  }

  summary.matchedRows = static_cast<uint32_t>(rows.size());
  summary.gatherSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - gatherStarted).count();

  // The order rows are applied in decides who wins a contested edge. Rule order
  // would make the first rule in the file always win; a shuffle from the seed
  // makes it fair and still reproducible.
  std::mt19937 order(settings.seed * 0x9e3779b1u ^ (currentTick * 0x85ebca6bu));

  std::shuffle(rows.begin(), rows.end(), order);

  const uint32_t verticesBefore = store.vertexCount();

  applier->setRound(currentTick);

  const auto applyStarted = std::chrono::steady_clock::now();

  const RewriteStats stats = applier->apply(rows);

  summary.applySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - applyStarted).count();

  summary.fired = stats.rulesFired + due.rulesFired;
  summary.scheduled = stats.scheduled;
  summary.guarded = stats.guarded;
  summary.declined = stats.declined;
  summary.stale = stats.stale;
  summary.rejected = stats.claimsRejected;
  summary.born = store.vertexCount() - verticesBefore;
  summary.population = population();

  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  summary.seconds = seconds;
  ticks.push_back(summary);

  simulatedSeconds += seconds;
  slowestTickSeconds = std::max(slowestTickSeconds, seconds);
  matchSeconds += summary.matchSeconds;
  gatherSeconds += summary.gatherSeconds;
  applySeconds += summary.applySeconds;
  programTicks += summary.programTicks;

  return stats.rulesFired + stats.scheduled + due.rulesFired;
}

void StoryRunner::run(const uint32_t quietTicks)
{
  uint32_t quiet = 0u;

  while (currentTick < settings.startWeek + settings.ticks)
  {
    if (step() == 0u)
    {
      if (++quiet >= quietTicks)
      {
        break;
      }
    }
    else
    {
      quiet = 0u;
    }
  }
}

std::string StoryRunner::weekPhrase(const uint32_t week, const uint32_t startWeek)
{
  static const char *const months[] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};
  static const char *const ordinal[] = {"first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth"};

  // The first tick is startWeek + 1, and it is week 0 of the calendar.
  const int64_t relative = static_cast<int64_t>(week) - static_cast<int64_t>(startWeek) - 1;
  const int64_t year = relative >= 0 ? relative / 52 : -((-relative + 51) / 52);
  const uint32_t inYear = static_cast<uint32_t>(relative - year * 52);
  const uint32_t month = inYear * 12u / 52u;
  const uint32_t monthStart = (month * 52u + 11u) / 12u;
  const uint32_t monthEnd = ((month + 1u) * 52u + 11u) / 12u;
  const uint32_t third = monthEnd > monthStart ? (inYear - monthStart) * 3u / (monthEnd - monthStart) : 1u;

  std::string phrase = std::string(third == 0u ? "early " : third == 1u ? "mid-" : "late ") + months[month];

  if (year == 1)
  {
    phrase += " of the following year";
  }
  else if (year > 1 && year <= 9)
  {
    phrase += std::string(" of the ") + ordinal[year] + " year";
  }
  else if (year > 9)
  {
    phrase += " of year " + std::to_string(year + 1);
  }
  else if (year < 0)
  {
    phrase += ", " + std::to_string(-year) + (year == -1 ? " year" : " years") + " before";
  }

  return phrase;
}

std::string StoryRunner::text()
{
  const std::vector<NarratedTick> narrated =
      narrator.narrate(log, program, nameTable, [this](const uint32_t id) { return genderOf(id); });

  std::ostringstream out;

  const uint32_t weeks = static_cast<uint32_t>(ticks.size());
  const uint32_t years = weeks / 52u;

  out << "Seed " << settings.seed << ". ";

  if (years == 0u)
  {
    out << weeks << " weeks";
  }
  else
  {
    out << years << (years == 1u ? " year" : " years");

    if (weeks % 52u != 0u)
    {
      out << " and " << (weeks % 52u) << " weeks";
    }
  }

  out << ", " << log.size() << " events.\n\n";

  uint32_t lastMonth = ~0u;

  for (const NarratedTick &tick : narrated)
  {
    appendNarratedTick(out, tick, settings.startWeek, settings.byMonth, &lastMonth);
  }

  return out.str();
}

std::string StoryRunner::monthPhrase(const uint32_t week, const uint32_t startWeek, uint32_t &key)
{
  static const char *const months[] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};
  static const char *const ordinal[] = {"first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth", "tenth"};

  const int64_t relative = static_cast<int64_t>(week) - static_cast<int64_t>(startWeek) - 1;

  // A week BEFORE the story's first one has a negative offset, and rounding
  // it towards zero left `inYear` negative and the month index off the end
  // of the table. Round down instead, the way weekPhrase does: the year is
  // negative and the week within it is still in [0, 52).
  const int64_t year = relative >= 0 ? relative / 52 : -((-relative + 51) / 52);
  const uint32_t inYear = static_cast<uint32_t>(relative - year * 52);
  const uint32_t month = inYear * 12u / 52u;

  key = static_cast<uint32_t>(static_cast<int64_t>(year) * 12 + static_cast<int64_t>(month));

  std::string phrase = months[month];

  if (year == 1)
  {
    phrase += " of the following year";
  }
  else if (year > 1 && year <= 9)
  {
    phrase += std::string(" of the ") + ordinal[year] + " year";
  }
  else if (year > 9)
  {
    phrase += " of year " + std::to_string(year + 1);
  }
  else if (year < 0)
  {
    phrase += ", " + std::to_string(-year) + (year == -1 ? " year" : " years") + " before";
  }

  return phrase;
}

void StoryRunner::appendNarratedTick(std::ostream &out, const NarratedTick &tick, const uint32_t startWeek, const bool byMonth,
                                     uint32_t *lastMonth)
{
  if (byMonth)
  {
    uint32_t key = 0u;
    const std::string month = monthPhrase(tick.tick, startWeek, key);

    if (lastMonth == nullptr || *lastMonth != key)
    {
      out << (lastMonth != nullptr && *lastMonth != ~0u ? "\n" : "") << "-- " << month << " --\n";

      if (lastMonth != nullptr)
      {
        *lastMonth = key;
      }
    }
  }
  else
  {
    out << "-- Week " << (tick.tick - startWeek) << ", " << weekPhrase(tick.tick, startWeek) << " --\n";
  }

  // One narration a line, joined by semicolons, with a full stop on the last:
  // a week of thirty events packed four to a line was a wall of text.
  for (size_t i = 0u; i < tick.sentences.size(); ++i)
  {
    std::string sentence = tick.sentences[i];

    if (i + 1u != tick.sentences.size() && !sentence.empty() && sentence.back() == '.')
    {
      sentence.back() = ';';
    }

    out << sentence << "\n";
  }

  out << (byMonth ? "" : "\n");
}

std::string StoryRunner::footer() const
{
  // What the run cost. Average per week, and the slowest week, because a
  // village that grows makes the last years dearer than the first. Kept out
  // of text() so the story itself is the same for the same seed.
  const uint32_t weeks = static_cast<uint32_t>(ticks.size());
  const double averageMs = ticks.empty() ? 0.0 : simulatedSeconds * 1000.0 / static_cast<double>(weeks);
  const double perProgramTickUs = programTicks == 0u ? 0.0 : matchSeconds * 1e6 / static_cast<double>(programTicks);
  const double share = simulatedSeconds > 0.0 ? 100.0 / simulatedSeconds : 0.0;

  std::ostringstream out;

  out << "-- The end --\n"
      << population() << " living at the close of " << weeks << " weeks.\n"
      << "Simulation (CPU): " << weeks << " ticks in " << std::fixed << std::setprecision(3) << simulatedSeconds << " s; "
      << std::setprecision(3) << averageMs << " ms per tick on average, " << std::setprecision(1) << slowestTickSeconds * 1000.0
      << " ms for the slowest.\n"
      << "Programme: " << programTicks << " instruction ticks in " << std::setprecision(3) << matchSeconds << " s; "
      << std::setprecision(2) << perProgramTickUs << " us per instruction tick, " << std::setprecision(1)
      << (weeks == 0u ? 0.0 : static_cast<double>(programTicks) / weeks) << " per simulation tick.\n"
      << "Where it went: match " << std::setprecision(0) << matchSeconds * share << "%, gather+dice " << gatherSeconds * share
      << "%, rewrite " << applySeconds * share << "%.\n";

  if (overflowedTicks != 0u)
  {
    out << "WARNING: the matcher overflowed a table in " << overflowedTicks << " of those weeks and lost matches; raise --rows.\n";
  }

  return out.str();
}

} // namespace graphrewrite::runtime
