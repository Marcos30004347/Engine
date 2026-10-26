#include "graphrewrite/compiler/UsageValues.hpp"

namespace graphrewrite::compiler {

UsageValues computeUsageValues(const CompiledProgram &program)
{
  UsageValues usage;

  usage.instructionCount = static_cast<uint32_t>(program.instructions.size());
  usage.edgeTypeCount = program.edgeTypes.size();

  if (usage.instructionCount == 0u || usage.edgeTypeCount == 0u)
  {
    return usage;
  }

  usage.nextUse.assign(static_cast<size_t>(usage.instructionCount) * usage.edgeTypeCount, UsageValues::never);

  // Sweep backwards carrying the row forward, overwriting only the entry for the
  // type this instruction reads. Bookkeeping instructions read nothing and so
  // simply inherit the row after them.
  std::vector<uint32_t> carried(usage.edgeTypeCount, UsageValues::never);

  for (uint32_t i = usage.instructionCount; i-- > 0u;)
  {
    const Instruction &instruction = program.instructions[i];

    if ((instruction.op == Op::Select || instruction.op == Op::Where) && instruction.label < usage.edgeTypeCount)
    {
      carried[instruction.label] = i;
    }

    for (uint32_t type = 0u; type < usage.edgeTypeCount; ++type)
    {
      usage.nextUse[static_cast<size_t>(i) * usage.edgeTypeCount + type] = carried[type];
    }
  }

  return usage;
}

std::string dumpUsageValues(const UsageValues &usage, const CompiledProgram &program)
{
  std::string out;

  for (uint32_t i = 0u; i < usage.instructionCount; ++i)
  {
    out += "i=" + std::to_string(i);

    for (uint32_t type = 0u; type < usage.edgeTypeCount; ++type)
    {
      const uint32_t value = usage.at(i, type);

      out += "  " + program.edgeTypes[type].pattern + "=" + (value == UsageValues::never ? std::string("never") : std::to_string(value));
    }

    out += '\n';
  }

  return out;
}

} // namespace graphrewrite::compiler
