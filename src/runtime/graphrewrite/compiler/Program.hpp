#pragma once

// The compiled programme: a linear instruction stream per merged graph.

#include "graphrewrite/compiler/EdgeTypeTable.hpp"
#include "graphrewrite/frontend/Ast.hpp"
#include "graphrewrite/compiler/PatternMatcher.hpp"
#include "graphrewrite/compiler/RuleGraph.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace graphrewrite::compiler {

enum class Op
{
  Select,        // seed a table from every edge of one type
  Where,         // extend the table by one pattern edge
  FreeTable,     // last use of a register
  ReportRewrite, // a rule's WHEN is now fully matched
};

const char *opName(Op op);

constexpr uint32_t noRegister = ~uint32_t(0);
constexpr uint32_t noRuleColumn = ~uint32_t(0);

// How one instruction lands in ONE rule's table.
//
// The merged graph's column indices are meaningless to a rule: each rule owns a
// table whose columns are its own vertices. An instruction serving three rules
// therefore writes into three tables at three different pairs of columns.
struct RuleColumns
{
  uint32_t ruleId = 0u;
  uint32_t fromColumn = 0u;
  uint32_t writeColumn = 0u;

  // Where to record the matched edge's (page, slot) for this rule, when the
  // rule will later remove that edge. The rewrite then never has to search the
  // graph again for an edge it already had in hand.
  uint32_t edgeColumn = noRuleColumn;
};

struct Instruction
{
  Op op = Op::Select;

  uint32_t reg = noRegister;  // register this instruction defines
  uint32_t from = noRegister; // register a Where extends, or the freed register

  EdgeTypeId label = invalidEdgeType;
  uint32_t origin = 0u; // merged-graph vertices
  uint32_t target = 0u;

  StepDirection direction = StepDirection::Forward;

  uint32_t fromColumn = 0u;
  uint32_t writeColumn = 0u;

  // Which rules this instruction serves. Empty is a defect: an instruction that
  // serves nobody is work nobody asked for.
  std::vector<uint32_t> rules;

  uint32_t rule = ~uint32_t(0); // ReportRewrite only

  uint32_t thread = 0u; // which merged graph this came from

  std::vector<RuleColumns> perRule;
};

// What a rule does once it has matched, in that rule's own vertex numbering.
//
// An edge the THEN clause restates survives; one it does not is removed. An edge
// the THEN clause adds is inserted. Vertices the THEN mentions but the WHEN does
// not are fresh and get ids from the vertex allocator at apply time.
struct RuleRewrite
{
  std::vector<PatternEdge> removals;
  std::vector<PatternEdge> insertions;

  // Every WHEN edge, clauses included. Guards are the WHEN edges with a clause,
  // evaluated before the row fires. The full list is what a SCHEDULED rewrite
  // re-validates when its time comes: a match made days ago may no longer hold.
  std::vector<PatternEdge> when;

  // Vertices numbered at or above this are new; below it they came from the
  // match and are read out of the table.
  uint32_t matchedVertexCount = 0u;
  uint32_t totalVertexCount = 0u;

  // Whether a matched row fires. ALWAYS is the default and what every rule did
  // before policies existed, so an old programme deserialises unchanged.
  RulePolicy policy;
};

struct CompiledProgram
{
  std::vector<Instruction> instructions;
  EdgeTypeTable edgeTypes;
  std::vector<std::string> ruleNames;

  // Instruction index where each merged graph's block begins.
  std::vector<uint32_t> threadStarts;

  // Total columns each rule's table needs: one per vertex, plus one per edge
  // the rule removes.
  std::vector<uint32_t> ruleColumnCounts;

  // How many merged edges the emitter was given, for reporting.
  uint32_t mergedEdgeCount = 0u;

  // WHEN edges per rule. The verifier checks each rule is served by exactly
  // this many queries, forming one table lineage.
  std::vector<uint32_t> ruleEdgeCounts;

  // Each rule's vertex names, in that rule's own vertex numbering - the names
  // the source used. A narration template's {A} resolves through this.
  std::vector<std::vector<std::string>> ruleVertexNames;

  // What each rule removes and inserts when it fires.
  std::vector<RuleRewrite> ruleRewrites;
};

std::string dumpProgram(const CompiledProgram &program);

} // namespace graphrewrite::compiler
