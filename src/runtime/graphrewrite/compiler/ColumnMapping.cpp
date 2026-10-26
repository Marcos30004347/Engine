#include "graphrewrite/compiler/ColumnMapping.hpp"

#include <algorithm>

namespace graphrewrite::compiler {
namespace {

// A merged vertex stands for one vertex in each rule it serves.
uint32_t ruleVertexOf(const MergedGraph &merged, const uint32_t mergedVertex, const uint32_t rule)
{
  const std::vector<uint32_t> &rules = merged.vertexRules[mergedVertex];

  for (uint32_t i = 0u; i < static_cast<uint32_t>(rules.size()); ++i)
  {
    if (rules[i] == rule && i < merged.vertexIndices[mergedVertex].size())
    {
      return merged.vertexIndices[mergedVertex][i];
    }
  }

  return noRuleColumn;
}

// True when the rule's THEN keeps this edge. An edge the THEN does not restate
// is removed, and a removed edge needs its provenance recorded.
bool preservedByThen(const RuleGraph &graph, const uint32_t origin, const EdgeTypeId label, const uint32_t target)
{
  for (const PatternEdge &edge : graph.then)
  {
    if (edge.origin == origin && edge.target == target && edge.label == label)
    {
      return true;
    }
  }

  return false;
}

} // namespace

bool assignRuleColumns(const std::vector<MergedGraph> &threads, const std::vector<RuleGraph> &graphs, CompiledProgram &program,
                       DiagnosticSink &sink)
{
  const uint32_t errorsBefore = sink.errorCount();

  // A rule's table starts with one column per vertex; provenance columns are
  // appended as the edges that need them are found.
  program.ruleColumnCounts.assign(graphs.size(), 0u);

  for (uint32_t rule = 0u; rule < static_cast<uint32_t>(graphs.size()); ++rule)
  {
    program.ruleColumnCounts[rule] = graphs[rule].vertexCount();
  }

  for (Instruction &instruction : program.instructions)
  {
    if (instruction.op != Op::Select && instruction.op != Op::Where)
    {
      continue;
    }

    if (instruction.thread >= threads.size())
    {
      SourceSpan span;

      sink.error(span, "instruction refers to thread " + std::to_string(instruction.thread) + " which does not exist");

      continue;
    }

    const MergedGraph &merged = threads[instruction.thread];

    // Recover which merged vertex each side of the step binds.
    const bool backward = instruction.direction == StepDirection::Backward;

    const uint32_t fromVertex = backward ? instruction.target : instruction.origin;
    const uint32_t writeVertex = backward ? instruction.origin : instruction.target;

    for (const uint32_t rule : instruction.rules)
    {
      RuleColumns columns;

      columns.ruleId = rule;
      columns.fromColumn = ruleVertexOf(merged, fromVertex, rule);
      columns.writeColumn = ruleVertexOf(merged, writeVertex, rule);

      if (columns.fromColumn == noRuleColumn || columns.writeColumn == noRuleColumn)
      {
        SourceSpan span;

        sink.error(span, "rule '" + graphs[rule].name + "' has no vertex for a merged vertex it is recorded on");

        continue;
      }

      if (columns.fromColumn >= graphs[rule].vertexCount() || columns.writeColumn >= graphs[rule].vertexCount())
      {
        SourceSpan span;

        sink.error(span, "rule '" + graphs[rule].name + "' column index out of range");

        continue;
      }

      if (!preservedByThen(graphs[rule], columns.fromColumn, instruction.label, columns.writeColumn) &&
          !preservedByThen(graphs[rule], columns.writeColumn, instruction.label, columns.fromColumn))
      {
        columns.edgeColumn = program.ruleColumnCounts[rule]++;
      }

      instruction.perRule.push_back(columns);
    }
  }

  return sink.errorCount() == errorsBefore;
}

} // namespace graphrewrite::compiler
