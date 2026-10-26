#include "graphrewrite/runtime/GpuStoryRunner.hpp"

#include "graphrewrite/runtime/Dice.hpp"
#include "graphrewrite/runtime/StoryClauses.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace graphrewrite::runtime {

GpuStoryRunner::GpuStoryRunner(::rendering::RHI *rhi, const compiler::CompiledProgram &compiled, const compiler::UsageValues &usage,
                               const LoadedFacts &world, const Narrator &teller, StorySettings storySettings,
                               GpuRunnerSettings gpuSettings)
    : program(compiled), narrator(teller), settings(std::move(storySettings))
{
  store.configure(program.edgeTypes);

  program.edgeTypes.find(settings.aliveType, alive);
  program.edgeTypes.find(settings.manType, man);
  program.edgeTypes.find(settings.womanType, woman);
  program.edgeTypes.find(settings.houseType, house);

  // The ceiling goes on BEFORE the facts are seeded, for the same reason as
  // on the CPU: a birth week is an edge to a node above dayBase.
  store.setVertexCeiling(settings.dayBase);

  controlStride = 1u + gpuSettings.maxFreshVertices;
  timers = gpuSettings.enableTimers;

  runner = std::make_unique<GpuProgramRunner>(rhi, program, usage, store, gpuSettings);
  runner->setPinFinalTables(false);
  runner->seed(world.facts);
  store.reserveVertices(static_cast<uint32_t>(world.names.size()));
  runner->createResources();

  applier = std::make_unique<RewriteApplier>(program, store, settings.seed);

  isDeathRule.assign(program.ruleNames.size(), false);
  needsClauses.assign(program.ruleNames.size(), false);
  deferredInsertions.assign(program.ruleNames.size(), {});

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    isDeathRule[rule] = ruleTitleOf(program.ruleNames[rule]) == settings.deathRule;

    if (rule >= program.ruleRewrites.size())
    {
      continue;
    }

    const compiler::RuleRewrite &rewrite = program.ruleRewrites[rule];

    bool guardsOrBinds = std::any_of(rewrite.when.begin(), rewrite.when.end(), [](const compiler::PatternEdge &e) { return e.hasClause; });

    deferredInsertions[rule].assign(rewrite.insertions.size(), false);

    for (size_t i = 0u; i < rewrite.insertions.size(); ++i)
    {
      const compiler::PatternEdge &edge = rewrite.insertions[i];

      if (!edge.hasClause)
      {
        continue;
      }

      if (edge.clause.kind == Expr::Kind::Bind)
      {
        guardsOrBinds = true;
      }
      else
      {
        deferredInsertions[rule][i] = true;
      }
    }

    needsClauses[rule] = guardsOrBinds;
  }

  applier->setFireObserver([this](const RewriteEvent &fired) {
    FiredEvent event;

    event.tick = currentTick;
    event.rule = fired.rule;
    event.bound = fired.bound;
    event.kind = fired.kind;

    record(event);
  });

  rolls.seed(settings.seed * 0x9e3779b1u + 7u);

  registerStoryClauses(host, settings.dayBase, booked, rolls);

  applier->setHost(&host);

  if (settings.recordEvents)
  {
    worker = std::make_unique<NarrationWorker>(narrator, program, settings.seed, world.names, settings.dayBase, settings.startWeek,
                                               settings.byMonth);
  }

  currentTick = settings.startWeek;
  currentPopulation = population();
}

void GpuStoryRunner::setChronicle(ChronicleSettings chronicle, const DialogueBook *book)
{
  if (worker == nullptr)
  {
    // No event feed, nothing to compose: --no-text turns the narrator off.
    return;
  }

  chronicle.nodeCeiling = settings.dayBase;

  worker->setChronicle(chronicle, book);
}

GpuStoryRunner::~GpuStoryRunner()
{
  if (worker)
  {
    worker->finish();
  }
}

uint32_t GpuStoryRunner::population() const
{
  return alive == compiler::invalidEdgeType ? 0u : store.edgeCount(alive);
}

uint32_t GpuStoryRunner::familyOf(const uint32_t id) const
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

Gender GpuStoryRunner::genderOf(const uint32_t id) const
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

uint32_t GpuStoryRunner::scaledChance(const uint32_t rule, const uint32_t permille) const
{
  if (rule >= isDeathRule.size() || !isDeathRule[rule] || settings.populationCap == 0u)
  {
    return permille;
  }

  const uint32_t quiet = permille / 10u;

  if (currentPopulation <= settings.populationCap)
  {
    return quiet;
  }

  const uint64_t excess = currentPopulation - settings.populationCap;
  const uint64_t pressure = excess * dice::permille / settings.populationCap;

  return static_cast<uint32_t>(std::min<uint64_t>(dice::permille, quiet + pressure));
}

bool GpuStoryRunner::ruleNeedsClauses(const uint32_t rule) const
{
  return rule < needsClauses.size() && needsClauses[rule];
}

void GpuStoryRunner::record(const FiredEvent &event)
{
  ++eventsSeen;

  if (worker)
  {
    weekEvents.push_back(event);
  }
}

uint32_t GpuStoryRunner::step()
{
  const auto started = std::chrono::steady_clock::now();

  ++currentTick;

  GpuTickSummary summary;

  summary.tick = currentTick;
  currentPopulation = population();

  applier->setNow(currentTick);
  applier->setRound(currentTick);

  // Today's date first, on the CPU store; if anything landed, the GPU pages
  // are stale and are rebuilt before the match (keeping what did not change).
  runner->snapshotResidency();

  const RewriteStats due = applier->applyDue();

  summary.released = due.released;
  summary.cancelled = due.cancelled;

  if (due.released != 0u)
  {
    runner->resyncStore();
  }

  // --- match and rewrite, on the GPU ---------------------------------------
  //
  // Each rule's rewrite is applied the moment its final table completes,
  // from the match callback, and its table's region is then free for the
  // instructions that follow. Rules are therefore applied in the order the
  // programme completes them; within a rule the claim pass decides, and a
  // later rule's rows are checked against the graph the earlier ones left.
  RewriteStats queueStats;
  double clauseSeconds = 0.0;
  double rewriteSeconds = 0.0;
  const uint32_t ruleCount = static_cast<uint32_t>(program.ruleNames.size());

  runner->setMatchCallback([this, &summary, &queueStats, &clauseSeconds, &rewriteSeconds](const uint32_t rule, const RuleResult &result) {
    if (rule >= program.ruleRewrites.size() || result.rows == 0u)
    {
      return;
    }

    const auto rewriteStarted = std::chrono::steady_clock::now();
    const compiler::RuleRewrite &rewrite = program.ruleRewrites[rule];

    RuleApplyOptions options;

    options.chance = scaledChance(rule, rewrite.policy.permille());
    options.deferredInsertions = deferredInsertions[rule];

    if (ruleNeedsClauses(rule))
    {
      options.decide = [this, rule, &rewrite, &summary, &clauseSeconds](const std::vector<CandidateRow> &candidates,
                                                                        std::vector<uint32_t> &control) {
        const auto clauseStarted = std::chrono::steady_clock::now();

        summary.candidates += static_cast<uint32_t>(candidates.size());

        for (const CandidateRow &candidate : candidates)
        {
          std::vector<uint32_t> bound = candidate.vertices;

          bound.resize(std::max<size_t>(bound.size(), rewrite.totalVertexCount), 0u);

          if (!applier->guardsHold(rule, bound))
          {
            continue;
          }

          applier->resolveBinds(rule, bound);

          const size_t base = static_cast<size_t>(candidate.row) * controlStride;

          if (base + controlStride > control.size())
          {
            continue;
          }

          control[base] = 1u;

          for (uint32_t k = 0u; k + 1u < controlStride && rewrite.matchedVertexCount + k < bound.size(); ++k)
          {
            control[base + 1u + k] = bound[rewrite.matchedVertexCount + k];
          }
        }

        clauseSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - clauseStarted).count();
      };
    }

    std::vector<FiredRecord> fired;

    const GpuRewriteStats stats = runner->applyRule(rule, options, &fired);

    summary.overflowed = summary.overflowed || stats.overflowed;
    overflows |= stats.overflowKinds;
    summary.claimsRejected += stats.claimsRejected;
    summary.fired += stats.rowsFired;

    const bool hasDeferred =
        std::any_of(deferredInsertions[rule].begin(), deferredInsertions[rule].end(), [](const bool deferred) { return deferred; });

    for (const FiredRecord &record : fired)
    {
      // The record carries the fresh ids the GPU used (a bind's id included),
      // so the schedule sees the same nodes the insertion did.
      if (hasDeferred)
      {
        applier->scheduleDeferred(rule, record.bound, record.row, queueStats);
      }

      applier->notifyApplied(rule, record.bound, record.row);
    }

    rewriteSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - rewriteStarted).count();
  });

  const auto matchStarted = std::chrono::steady_clock::now();

  runner->reset();

  if (!runner->runToCompletion())
  {
    summary.overflowed = true;
    overflows |= 16u;
    ++overflowedTicks;
  }

  runner->setMatchCallback(MatchCallback());

  summary.programTicks = runner->stats().instructionsCompleted;
  summary.submits = runner->stats().ticks;
  summary.submitWaitMs = runner->stats().msSubmitWait;

  const double wholeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - matchStarted).count();

  (void)ruleCount;
  summary.matchSeconds = wholeSeconds - rewriteSeconds;
  summary.scheduled = queueStats.scheduled;
  summary.clauseSeconds = clauseSeconds;
  summary.rewriteSeconds = rewriteSeconds - clauseSeconds;

  // --- hand the week to the narrator --------------------------------------
  if (worker && !weekEvents.empty())
  {
    std::vector<NarrationWorker::NodeFacts> facts;
    std::set<uint32_t> seen;

    for (const FiredEvent &event : weekEvents)
    {
      for (const uint32_t id : event.bound)
      {
        if (id < settings.dayBase && seen.insert(id).second)
        {
          facts.push_back(NarrationWorker::NodeFacts{id, genderOf(id), familyOf(id)});
        }
      }
    }

    worker->push(currentTick, std::move(weekEvents), std::move(facts));
    weekEvents.clear();
  }

  for (const FrameTiming &frame : runner->frames())
  {
    frameLog.push_back(WeekFrame{currentTick - settings.startWeek, frame});
  }

  runner->clearFrames();

  // Read before the next week's reset() clears them.
  peakRows = std::max(peakRows, runner->stats().peakRowsNeeded);
  peakRegions = std::max(peakRegions, runner->stats().peakRegionsUsed);
  regionsAvailable = runner->stats().regionsAvailable;

  summary.pagesAdmitted = runner->stats().pagesAdmitted;
  summary.serviceMs = runner->stats().msService;

  summary.population = population();
  summary.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  simulatedSeconds += summary.seconds;
  this->matchSeconds += summary.matchSeconds;
  this->rewriteSeconds += summary.rewriteSeconds; // the local of the same name is this week's
  this->clauseSeconds += summary.clauseSeconds;
  submitWaitMs += summary.submitWaitMs;
  serviceMs += summary.serviceMs;
  pagesAdmitted += summary.pagesAdmitted;
  programTicks += summary.programTicks;
  submits += summary.submits;

  ticks.push_back(summary);

  return summary.fired + summary.scheduled + summary.released;
}

void GpuStoryRunner::run(const uint32_t quietTicks)
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

std::string GpuStoryRunner::text()
{
  if (!worker)
  {
    return "Seed " + std::to_string(settings.seed) + ". Narration off.\n";
  }

  if (!joined)
  {
    worker->finish();
    finished = worker->text();
    joined = true;
  }

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

  out << ", " << eventsSeen << " events, simulated on the GPU.\n\n" << finished;

  return out.str();
}

std::string GpuStoryRunner::footer()
{
  const uint32_t weeks = static_cast<uint32_t>(ticks.size());
  const double averageMs = weeks == 0u ? 0.0 : simulatedSeconds * 1000.0 / weeks;
  const double perProgramTickUs = programTicks == 0u ? 0.0 : matchSeconds * 1e6 / static_cast<double>(programTicks);
  const double share = simulatedSeconds > 0.0 ? 100.0 / simulatedSeconds : 0.0;

  double slowest = 0.0;

  for (const GpuTickSummary &tick : ticks)
  {
    slowest = std::max(slowest, tick.seconds);
  }

  std::ostringstream out;

  out << "-- The end --\n"
      << population() << " living at the close of " << weeks << " weeks.\n"
      << "Simulation (GPU): " << weeks << " ticks in " << std::fixed << std::setprecision(3) << simulatedSeconds << " s; "
      << std::setprecision(3) << averageMs << " ms per tick on average, " << std::setprecision(1) << slowest * 1000.0
      << " ms for the slowest.\n"
      << "Programme: " << programTicks << " instruction ticks in " << std::setprecision(3) << matchSeconds << " s; "
      << std::setprecision(2) << perProgramTickUs << " us per instruction tick, " << std::setprecision(1)
      << (weeks == 0u ? 0.0 : static_cast<double>(programTicks) / weeks) << " per simulation tick; " << submits
      << " GPU submits in all, " << std::setprecision(1) << submitWaitMs / 1000.0 << " s of it waiting on the device.\n"
      << "Where it went: match " << std::setprecision(0) << matchSeconds * share << "%, rewrite " << rewriteSeconds * share
      << "%, clauses on the CPU " << clauseSeconds * share << "%.\n";

  if (!frameLog.empty())
  {
    double wall = 0.0;
    double gpu = 0.0;
    double matchWall = 0.0;
    double matchGpu = 0.0;
    uint32_t matchFrames = 0u;
    double slowest = 0.0;

    for (const WeekFrame &entry : frameLog)
    {
      wall += entry.frame.wallMs;
      gpu += entry.frame.gpuMs;
      slowest = std::max(slowest, entry.frame.wallMs);

      if (entry.frame.kind == 0u)
      {
        ++matchFrames;
        matchWall += entry.frame.wallMs;
        matchGpu += entry.frame.gpuMs;
      }
    }

    const double n = static_cast<double>(frameLog.size());

    out << "Frames (one GPU frame = one tick of the programme runner): " << frameLog.size() << "; " << std::setprecision(3)
        << wall / n << " ms wall per frame on average, " << std::setprecision(1) << slowest << " ms the slowest; match frames "
        << matchFrames << " at " << std::setprecision(3) << (matchFrames ? matchWall / matchFrames : 0.0) << " ms.\n";

    if (timers)
    {
      out << "GPU time from the graph's timestamps: " << std::setprecision(3) << gpu / n << " ms per frame ("
          << (matchFrames ? matchGpu / matchFrames : 0.0) << " ms per match frame); the rest of the wall time is submit and wait.\n";
    }
  }

  if (pagesAdmitted != 0u)
  {
    out << "Page traffic: " << pagesAdmitted << " page uploads in " << std::setprecision(2) << serviceMs / 1000.0
        << " s between frames (" << std::setprecision(1) << static_cast<double>(pagesAdmitted) / std::max<size_t>(1u, ticks.size())
        << " per week).\n";
  }

  if (worker)
  {
    out << "Narration: " << std::setprecision(2) << worker->busySeconds() << " s on its own thread.\n";
  }

  if (overflowedTicks != 0u)
  {
    out << "WARNING: a buffer overflowed in " << overflowedTicks << " weeks and matches or firings were lost:";

    if ((overflows & 16u) != 0u)
    {
      out << " the match table (largest needed " << peakRows << " rows against --rows, and " << peakRegions << " of "
          << regionsAvailable << " table regions);";
    }

    if ((overflows & 1u) != 0u)
    {
      out << " the dirty-page list (--dirty, one entry per page a rule touches);";
    }

    if ((overflows & 2u) != 0u)
    {
      out << " the insertion delta;";
    }

    if ((overflows & 4u) != 0u)
    {
      out << " the candidate records;";
    }

    if ((overflows & 8u) != 0u)
    {
      out << " the fired records;";
    }

    out << "\n";
  }

  return out.str();
}

} // namespace graphrewrite::runtime
