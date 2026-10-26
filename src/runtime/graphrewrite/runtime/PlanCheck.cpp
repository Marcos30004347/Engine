#include "graphrewrite/runtime/PlanCheck.hpp"

#include <algorithm>

namespace graphrewrite::runtime {

PlanCheck checkProgramPlan(const compiler::CompiledProgram &program, const EdgePageStore &store)
{
  PlanCheck check;

  const std::vector<uint32_t> counts = store.edgeCounts(program.edgeTypes.size());

  for (compiler::EdgeTypeId type = 0u; type < program.edgeTypes.size(); ++type)
  {
    if (program.edgeTypes[type].assumedCount != counts[type])
    {
      check.staleStatistics.push_back(type);
    }
  }

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    // The seed is the Select on this rule's path; every other query extends it.
    const compiler::Instruction *seed = nullptr;
    std::vector<compiler::EdgeTypeId> reachable;

    for (const compiler::Instruction &instruction : program.instructions)
    {
      if (instruction.op != compiler::Op::Select && instruction.op != compiler::Op::Where)
      {
        continue;
      }

      if (std::find(instruction.rules.begin(), instruction.rules.end(), rule) == instruction.rules.end())
      {
        continue;
      }

      reachable.push_back(instruction.label);

      if (instruction.op == compiler::Op::Select && seed == nullptr)
      {
        seed = &instruction;
      }
    }

    if (seed == nullptr || reachable.empty())
    {
      continue;
    }

    const uint32_t seedCount = seed->label < counts.size() ? counts[seed->label] : 0u;

    compiler::EdgeTypeId rarest = reachable.front();
    uint32_t rarestCount = rarest < counts.size() ? counts[rarest] : 0u;

    for (const compiler::EdgeTypeId label : reachable)
    {
      const uint32_t candidate = label < counts.size() ? counts[label] : 0u;

      if (candidate < rarestCount)
      {
        rarest = label;
        rarestCount = candidate;
      }
    }

    check.plannedSeedRows += seedCount;
    check.bestSeedRows += rarestCount;

    if (rarestCount < seedCount)
    {
      PlanCheck::Mismatch mismatch;

      mismatch.rule = rule;
      mismatch.plannedSeed = seed->label;
      mismatch.plannedSeedCount = seedCount;
      mismatch.rarestAvailable = rarest;
      mismatch.rarestCount = rarestCount;

      check.suboptimalSeeds.push_back(mismatch);
    }
  }

  return check;
}

} // namespace graphrewrite::runtime
