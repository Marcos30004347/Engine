#pragma once

// A programme runner is its own little renderer: it owns its resources and is
// driven by tick(). One tick is one unit of work - for the GPU runner, one run
// of its render graph; for the CPU runner, the same step done in memory.
//
// Both implement the same interface so a run can be compared directly against
// the reference rather than only at the end.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"

#include <cstdint>
#include <functional>
#include <tuple>
#include <vector>

namespace graphrewrite::runtime {

struct TickResult
{
  // The instruction this tick worked on.
  uint32_t instruction = 0u;

  // True once every instruction of the programme has finished.
  bool programComplete = false;

  // True when the instruction this tick worked on finished during it.
  bool instructionComplete = false;

  uint32_t rowsProduced = 0u;
  uint32_t stalls = 0u;

  bool overflowed = false;
  bool starved = false;

};

struct RunnerStats
{
  uint32_t ticks = 0u;
  uint32_t instructionsCompleted = 0u;
  uint32_t stalls = 0u;
  uint32_t pagesAdmitted = 0u;

  // Separates driver round trips from bytes: a batch that collapses to few
  // writes is cheap however many pages it carries.
  uint64_t uploadWrites = 0u;
  uint64_t metadataFlushes = 0u;

  // Where a tick's wall time actually goes. A tick submits the graph and then
  // WAITS on it, so these separate "the GPU was busy" from "the CPU was blocked
  // on a round trip" - a distinction that pool size, edges-per-thread and
  // dispatch count all leave untouched, and which therefore has to be measured
  // rather than inferred from them.
  double msSubmitWait = 0.0;
  double msStatusRead = 0.0;
  double msService = 0.0;
  bool overflowed = false;
  bool starved = false;
  // The largest row count any instruction COUNTED this run, whether or not
  // the table could hold it. On overflow this is what the capacity should
  // have been - a run that has to be sized by hand should not make the
  // caller guess.
  uint32_t peakRowsNeeded = 0u;

  // GPU only: table regions handed out at once, and how many the buffer
  // holds. A static walk of the programme decides the second; when the first
  // passes it, the far end of a week silently produces nothing, so it is
  // reported rather than inferred.
  uint32_t peakRegionsUsed = 0u;
  uint32_t regionsAvailable = 0u;
};

// What a pre-flight check found about the plan the programme was compiled with.
//
// A programme is planned against assumed cardinalities. If the graph it is about
// to run on looks nothing like that, the plan is still CORRECT but can be badly
// slower: a traversal seeded from what is now the commonest edge type starts
// with a table of that size, and every later step multiplies out of it.
struct PlanCheck
{
  struct Mismatch
  {
    uint32_t rule = 0u;
    EdgeTypeId plannedSeed = 0u;
    uint32_t plannedSeedCount = 0u;
    EdgeTypeId rarestAvailable = 0u;
    uint32_t rarestCount = 0u;
  };

  // A rule whose seed is not the rarest edge it could have started from.
  std::vector<Mismatch> suboptimalSeeds;

  // Types whose real size differs from what the plan assumed.
  std::vector<EdgeTypeId> staleStatistics;

  // Rows the plan would seed with, against the best achievable.
  uint64_t plannedSeedRows = 0u;
  uint64_t bestSeedRows = 0u;

  bool optimal() const
  {
    return suboptimalSeeds.empty();
  }
};

struct RuleResult
{
  uint32_t rows = 0u;
  uint32_t columns = 0u;
  bool overflowed = false;
};

using Fact = std::tuple<uint32_t, EdgeTypeId, uint32_t>;

// Called when a pattern's last query finishes, with however many rows it
// matched. This is the hook for acting on a match without polling: the GPU
// runner invokes it after reading the completed table's row count back, which is
// the same readback the rewrite decision uses.
using MatchCallback = std::function<void(uint32_t rule, const RuleResult &result)>;

class IProgramRunner
{
public:
  virtual ~IProgramRunner() = default;

  // Invoked once per rule, when that rule's last query completes. A rule that
  // matched nothing is reported too, with zero rows, so a caller can tell "no
  // matches" from "not finished".
  virtual void setMatchCallback(MatchCallback callback) = 0;

  // Adds facts to the graph. Safe to call before the first tick.
  virtual void seed(const std::vector<Fact> &facts) = 0;

  // Restarts the programme from its first instruction.
  virtual void reset() = 0;

  // One unit of work.
  virtual TickResult tick() = 0;

  virtual bool complete() const = 0;

  // Runs ticks until the programme completes or the budget is exhausted.
  virtual bool runToCompletion(uint32_t maxTicks = 100000u);

  virtual RuleResult resultForRule(uint32_t rule) const = 0;

  virtual const RunnerStats &stats() const = 0;

  // Compares the plan's assumptions against the graph as it stands. Cheap, and
  // worth doing before a run rather than wondering afterwards why a step was
  // slow.
  virtual PlanCheck checkPlan() const = 0;

  // Every edge currently in the graph, sorted. For comparing two runners.
  virtual std::vector<Fact> snapshot() const = 0;
};

} // namespace graphrewrite::runtime
