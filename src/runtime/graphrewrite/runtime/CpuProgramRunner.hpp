#pragma once

// The CPU runner: the same instruction stream, the same paged store, done in
// memory. This is the reference the GPU runner is checked against, so it is
// written to be obviously correct rather than fast.

#include "graphrewrite/compiler/UsageValues.hpp"
#include "graphrewrite/runtime/IProgramRunner.hpp"
#include "graphrewrite/runtime/MatchTable.hpp"
#include "graphrewrite/runtime/WorkerPool.hpp"

#include <map>
#include <memory>

namespace graphrewrite::runtime {

class CpuProgramRunner : public IProgramRunner
{
public:
  struct Settings
  {
    uint32_t rowCapacityPerTable = 2048u;

    // Additional worker threads for the row loop. 0 runs everything on the
    // calling thread. The result is identical either way - chunks are
    // contiguous and concatenated in order - so this only changes how long it
    // takes.
    uint32_t workers = 0u;
  };

  CpuProgramRunner(const compiler::CompiledProgram &program, const compiler::UsageValues &usage, EdgePageStore &store, Settings settings);

  void seed(const std::vector<Fact> &facts) override;
  void setMatchCallback(MatchCallback callback) override;
  void reset() override;
  TickResult tick() override;
  bool complete() const override;
  RuleResult resultForRule(uint32_t rule) const override;
  const RunnerStats &stats() const override;
  PlanCheck checkPlan() const override;
  std::vector<Fact> snapshot() const override;

  const MatchTable *tableForRegister(uint32_t reg) const;

  // The table the LAST query serving a rule produced - the rule's finished
  // match set.
  const MatchTable *finalTableForRule(uint32_t rule) const;

private:
  // A CPU tick does one whole instruction: there is nothing to stall on, so
  // splitting it across ticks would only model the GPU's paging without
  // exercising anything.
  bool runInstruction(uint32_t index);
  void notifyCompletedRules(uint32_t index);

  // Columns a register table of this thread needs: one past the highest
  // column any of the thread's instructions reads or writes. Tables are laid
  // out in MERGED-graph columns, shared by every rule in the thread - sizing
  // one by a single rule's column count wrote past its cells whenever the
  // merged index was larger, which with many rules in one thread it always is.
  uint32_t columnsForThread(uint32_t thread) const;

  const compiler::CompiledProgram &program;
  const compiler::UsageValues &usage;
  EdgePageStore &store;
  Settings settings;

  std::map<uint32_t, MatchTable> registers;
  std::map<uint32_t, uint32_t> boundMasks;

  uint32_t cursor = 0u;
  std::unique_ptr<WorkerPool> pool;
  MatchCallback matchCallback;
  RunnerStats runnerStats;
};

} // namespace graphrewrite::runtime
