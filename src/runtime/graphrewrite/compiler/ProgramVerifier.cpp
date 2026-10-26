#include "graphrewrite/compiler/ProgramVerifier.hpp"

#include <map>
#include <set>

namespace graphrewrite::compiler {
namespace {

void report(DiagnosticSink &sink, const uint32_t index, const std::string &message)
{
  SourceSpan span;

  sink.error(span, "instruction " + std::to_string(index) + ": " + message);
}

void checkRegisters(const CompiledProgram &program, DiagnosticSink &sink)
{
  std::set<uint32_t> defined;
  std::set<uint32_t> freed;

  for (uint32_t i = 0u; i < static_cast<uint32_t>(program.instructions.size()); ++i)
  {
    const Instruction &instruction = program.instructions[i];

    if (instruction.op == Op::Where)
    {
      if (defined.count(instruction.from) == 0u)
      {
        report(sink, i, "WHERE reads undefined register " + std::to_string(instruction.from));
      }
      else if (freed.count(instruction.from) != 0u)
      {
        report(sink, i, "WHERE reads register " + std::to_string(instruction.from) + " after it was freed");
      }
    }

    if (instruction.op == Op::FreeTable)
    {
      if (defined.count(instruction.from) == 0u)
      {
        report(sink, i, "FREE of undefined register " + std::to_string(instruction.from));
      }
      else if (!freed.insert(instruction.from).second)
      {
        report(sink, i, "register " + std::to_string(instruction.from) + " freed twice");
      }
    }

    if (instruction.reg != noRegister && (instruction.op == Op::Select || instruction.op == Op::Where))
    {
      defined.insert(instruction.reg);
    }
  }

  for (const uint32_t reg : defined)
  {
    if (freed.count(reg) == 0u)
    {
      SourceSpan span;

      sink.error(span, "register " + std::to_string(reg) + " is never freed");
    }
  }
}

// The table an instruction extends must have been filtered ONLY by edges that
// every rule this instruction serves also requires. Otherwise a rule inherits
// some other rule's filter and silently loses matches it should have had.
//
// Expressed on the instruction stream: rules(i) must be a subset of
// rules(from(i)). Applied transitively that makes every ancestor's rule set a
// superset too, which is the real condition.
void checkTableLineage(const CompiledProgram &program, DiagnosticSink &sink)
{
  std::map<uint32_t, std::set<uint32_t>> rulesOfRegister;

  for (uint32_t i = 0u; i < static_cast<uint32_t>(program.instructions.size()); ++i)
  {
    const Instruction &instruction = program.instructions[i];

    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    const std::set<uint32_t> mine(instruction.rules.begin(), instruction.rules.end());

    if (instruction.op == Op::Where)
    {
      const auto parent = rulesOfRegister.find(instruction.from);

      if (parent != rulesOfRegister.end())
      {
        for (const uint32_t rule : mine)
        {
          if (parent->second.count(rule) == 0u)
          {
            report(sink, i, "extends a table filtered for rules rule " + std::to_string(rule) +
                                " never asked for - that rule would lose matches");

            break;
          }
        }
      }
    }

    rulesOfRegister[instruction.reg] = mine;
  }
}

void checkColumns(const CompiledProgram &program, DiagnosticSink &sink)
{
  for (uint32_t i = 0u; i < static_cast<uint32_t>(program.instructions.size()); ++i)
  {
    const Instruction &instruction = program.instructions[i];

    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    if (instruction.rules.empty())
    {
      report(sink, i, "instruction serves no rule");
    }

    if (instruction.perRule.size() != instruction.rules.size())
    {
      report(sink, i, "has " + std::to_string(instruction.perRule.size()) + " column mappings for " + std::to_string(instruction.rules.size()) +
                          " rules");
    }

    for (const RuleColumns &columns : instruction.perRule)
    {
      if (columns.ruleId >= program.ruleColumnCounts.size())
      {
        report(sink, i, "column mapping names rule " + std::to_string(columns.ruleId) + " which does not exist");

        continue;
      }

      const uint32_t width = program.ruleColumnCounts[columns.ruleId];

      if (columns.fromColumn >= width || columns.writeColumn >= width)
      {
        report(sink, i, "column mapping for rule " + std::to_string(columns.ruleId) + " is out of range");
      }

      if (columns.edgeColumn != noRuleColumn && columns.edgeColumn >= width)
      {
        report(sink, i, "provenance column for rule " + std::to_string(columns.ruleId) + " is out of range");
      }
    }
  }
}

void checkReports(const CompiledProgram &program, DiagnosticSink &sink)
{
  std::map<uint32_t, uint32_t> counts;

  for (const Instruction &instruction : program.instructions)
  {
    if (instruction.op == Op::ReportRewrite)
    {
      ++counts[instruction.rule];
    }
  }

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleNames.size()); ++rule)
  {
    const auto entry = counts.find(rule);
    const uint32_t seen = entry == counts.end() ? 0u : entry->second;

    if (seen != 1u)
    {
      SourceSpan span;

      sink.error(span, "rule '" + program.ruleNames[rule] + "' is reported " + std::to_string(seen) + " times, expected exactly once");
    }
  }
}

// Every rule must be served by exactly as many queries as it has WHEN edges,
// and those queries must form ONE root-to-node path. A rule whose edges are
// split across two roots has no table lineage, so it can never be matched -
// and nothing else reports it.
void checkRuleCoverage(const CompiledProgram &program, DiagnosticSink &sink)
{
  if (program.ruleEdgeCounts.empty())
  {
    return;
  }

  std::map<uint32_t, std::vector<uint32_t>> served; // rule -> instruction indices
  std::map<uint32_t, uint32_t> registerToIndex;

  for (uint32_t i = 0u; i < static_cast<uint32_t>(program.instructions.size()); ++i)
  {
    const Instruction &instruction = program.instructions[i];

    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    registerToIndex[instruction.reg] = i;

    for (const uint32_t rule : instruction.rules)
    {
      served[rule].push_back(i);
    }
  }

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(program.ruleEdgeCounts.size()); ++rule)
  {
    const auto entry = served.find(rule);
    const std::vector<uint32_t> &path = entry == served.end() ? std::vector<uint32_t>() : entry->second;

    if (path.size() != program.ruleEdgeCounts[rule])
    {
      SourceSpan span;

      sink.error(span, "rule '" + program.ruleNames[rule] + "' has " + std::to_string(program.ruleEdgeCounts[rule]) +
                           " WHEN edge(s) but is served by " + std::to_string(path.size()) + " quer(ies)");

      continue;
    }

    // The path must be a single lineage: exactly one root, each later
    // instruction extending the one before it.
    uint32_t roots = 0u;

    for (uint32_t step = 0u; step < static_cast<uint32_t>(path.size()); ++step)
    {
      const Instruction &instruction = program.instructions[path[step]];

      if (instruction.op == Op::Select)
      {
        ++roots;

        continue;
      }

      const auto parent = registerToIndex.find(instruction.from);

      if (parent == registerToIndex.end() || step == 0u || parent->second != path[step - 1u])
      {
        SourceSpan span;

        sink.error(span, "rule '" + program.ruleNames[rule] + "' is served by queries that are not one table lineage");

        break;
      }
    }

    if (roots != 1u)
    {
      SourceSpan span;

      sink.error(span, "rule '" + program.ruleNames[rule] + "' is served by " + std::to_string(roots) +
                           " roots; its edges must lie on one path");
    }
  }
}

void checkUsage(const CompiledProgram &program, const UsageValues &usage, DiagnosticSink &sink)
{
  if (usage.instructionCount != program.instructions.size())
  {
    SourceSpan span;

    sink.error(span, "usage values cover " + std::to_string(usage.instructionCount) + " instructions but the programme has " +
                         std::to_string(program.instructions.size()));

    return;
  }

  for (uint32_t i = 0u; i < usage.instructionCount; ++i)
  {
    for (uint32_t type = 0u; type < usage.edgeTypeCount; ++type)
    {
      const uint32_t value = usage.at(i, type);

      if (value != UsageValues::never && value < i)
      {
        report(sink, i, "usage value for edge type " + std::to_string(type) + " points backwards");
      }
    }
  }
}

} // namespace

bool verifyProgram(const CompiledProgram &program, const UsageValues &usage, DiagnosticSink &sink)
{
  const uint32_t errorsBefore = sink.errorCount();

  checkRegisters(program, sink);
  checkTableLineage(program, sink);
  checkColumns(program, sink);
  checkReports(program, sink);
  checkRuleCoverage(program, sink);
  checkUsage(program, usage, sink);

  return sink.errorCount() == errorsBefore;
}

} // namespace graphrewrite::compiler
