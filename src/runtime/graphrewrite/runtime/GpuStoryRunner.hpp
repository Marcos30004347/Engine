#pragma once

// The story, simulated on the GPU.
//
// Every week: the CPU queue lands what was scheduled for today (and the GPU
// pages are resynced if anything landed); the GPU matches every rule; then
// rule by rule the GPU applies the rewrite - dice, staleness, claims,
// removals, insertions, compaction - and reports each row that fired as a
// small record the CPU reads back. The CPU's part is the parenthesised
// clauses: for a rule with a guard or a bind, the GPU first hands over the
// rows its dice and staleness kept, the CPU evaluates the clauses on those
// rows only and answers with a verdict word per row plus the ids any bind
// named, and the GPU applies that verdict. Schedules never reach the GPU:
// an insertion with a schedule clause is queued on the CPU with the WHEN
// edges the rule kept, exactly as the CPU applier does it.
//
// The fired records are the narrator's feed. They go to a NarrationWorker
// on another thread, a week at a time; the simulation never waits for prose.
// With `setChronicle` that worker composes months in paragraphs instead of
// listing weeks - the only form that is readable at this scale.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/compiler/UsageValues.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"
#include "graphrewrite/runtime/ExpressionHost.hpp"
#include "graphrewrite/runtime/FactsFile.hpp"
#include "graphrewrite/runtime/GpuProgramRunner.hpp"
#include "graphrewrite/runtime/Chronicler.hpp"
#include "graphrewrite/runtime/DialogueBook.hpp"
#include "graphrewrite/runtime/NarrationWorker.hpp"
#include "graphrewrite/runtime/Narrator.hpp"
#include "graphrewrite/runtime/RewriteApplier.hpp"
#include "graphrewrite/runtime/StoryRunner.hpp"

#include <memory>
#include <random>
#include <set>

namespace rendering {
class RHI;
}

namespace graphrewrite::runtime {

struct GpuTickSummary
{
  uint32_t tick = 0u;
  uint32_t population = 0u;
  uint32_t fired = 0u;
  uint32_t scheduled = 0u;
  uint32_t released = 0u;
  uint32_t cancelled = 0u;
  uint32_t claimsRejected = 0u;
  uint32_t candidates = 0u; // rows the CPU judged clauses on
  uint32_t programTicks = 0u;
  uint32_t submits = 0u; // GPU frames this week: match instructions + rewrite phases
  uint32_t pagesAdmitted = 0u; // pages uploaded to the pool this week
  bool overflowed = false;

  double seconds = 0.0;
  double matchSeconds = 0.0;
  double rewriteSeconds = 0.0; // GPU rewrite frames, readbacks and the fold
  double clauseSeconds = 0.0;  // CPU clause evaluation inside the two-phase rules
  double submitWaitMs = 0.0;   // of which, waiting on the device
  double serviceMs = 0.0;      // page uploads, between frames
};

class GpuStoryRunner
{
public:
  GpuStoryRunner(::rendering::RHI *rhi, const compiler::CompiledProgram &program, const compiler::UsageValues &usage,
                 const LoadedFacts &world, const Narrator &narrator, StorySettings settings, GpuRunnerSettings gpuSettings);
  ~GpuStoryRunner();

  // Compose the story month by month rather than week by week. `book` may
  // be null. Call before the first step().
  void setChronicle(ChronicleSettings settings, const DialogueBook *book);

  uint32_t step();
  void run(uint32_t quietTicks = 52u);

  // Waits for the narration thread and returns the story.
  std::string text();
  std::string footer();

  uint32_t population() const;
  Gender genderOf(uint32_t id) const;
  uint32_t familyOf(uint32_t id) const;

  const std::vector<GpuTickSummary> &summaries() const
  {
    return ticks;
  }

  // Every GPU frame of the run, with the week it belongs to, when
  // GpuRunnerSettings::recordFrames is on.
  struct WeekFrame
  {
    uint32_t week = 0u;
    FrameTiming frame;
  };

  const std::vector<WeekFrame> &frames() const
  {
    return frameLog;
  }

  bool timersOn() const
  {
    return timers;
  }

  uint32_t eventCount() const
  {
    return eventsSeen;
  }

  double elapsedSeconds() const
  {
    return simulatedSeconds;
  }

  uint32_t overflowedWeeks() const
  {
    return overflowedTicks;
  }

  // The largest table any instruction needed. On overflow, what --rows
  // should have been.
  uint32_t peakRowsNeeded() const
  {
    return peakRows;
  }

  // Which buffers ran out over the run: 1 dirty pages, 2 delta, 4
  // candidates, 8 fired records, 16 the match table.
  uint32_t overflowKinds() const
  {
    return overflows;
  }

private:
  uint32_t scaledChance(uint32_t rule, uint32_t permille) const;
  bool ruleNeedsClauses(uint32_t rule) const;
  void record(const FiredEvent &event);

  const compiler::CompiledProgram &program;
  const Narrator &narrator;
  StorySettings settings;
  EdgePageStore store;
  std::unique_ptr<GpuProgramRunner> runner;
  std::unique_ptr<RewriteApplier> applier;
  std::unique_ptr<NarrationWorker> worker;
  ExpressionHost host;
  std::set<std::pair<uint32_t, uint32_t>> booked;
  std::mt19937 rolls;

  compiler::EdgeTypeId alive = compiler::invalidEdgeType;
  compiler::EdgeTypeId man = compiler::invalidEdgeType;
  compiler::EdgeTypeId woman = compiler::invalidEdgeType;
  compiler::EdgeTypeId house = compiler::invalidEdgeType;
  std::vector<bool> isDeathRule;
  std::vector<bool> needsClauses;
  std::vector<std::vector<bool>> deferredInsertions;

  uint32_t controlStride = 1u;
  uint32_t currentTick = 0u;
  uint32_t currentPopulation = 0u;
  std::vector<GpuTickSummary> ticks;
  std::vector<FiredEvent> weekEvents;
  uint32_t eventsSeen = 0u;
  uint32_t overflowedTicks = 0u;
  uint32_t peakRows = 0u;
  uint32_t overflows = 0u;
  uint32_t peakRegions = 0u;
  uint32_t regionsAvailable = 0u;
  double simulatedSeconds = 0.0;
  double matchSeconds = 0.0;
  double rewriteSeconds = 0.0;
  double clauseSeconds = 0.0;
  double submitWaitMs = 0.0;
  uint64_t programTicks = 0u;
  uint64_t submits = 0u;
  uint64_t pagesAdmitted = 0u;
  double serviceMs = 0.0;
  std::string finished;
  bool joined = false;
  std::vector<WeekFrame> frameLog;
  bool timers = false;
};

} // namespace graphrewrite::runtime
