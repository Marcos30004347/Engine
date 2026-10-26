#pragma once

// Turns a rule's finished match table into rows in the RULE'S OWN vertex
// numbering.
//
// A table's columns are the merged graph's: several rules share one table, and
// each rule's vertex 0 can sit in a different column. The compiler records the
// translation on every instruction (`Instruction::perRule`), but nothing in the
// runtime read it - both runners assumed column i was rule vertex i, which holds
// only while the rule was merged in first. This is the one place that assumption
// is replaced by the mapping.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/CpuProgramRunner.hpp"
#include "graphrewrite/runtime/RewriteApplier.hpp"

#include <functional>
#include <vector>

namespace graphrewrite::runtime {

// rule vertex -> merged table column, or noRuleColumn for a vertex the WHEN
// never binds (one the THEN creates).
std::vector<uint32_t> ruleVertexColumns(const compiler::CompiledProgram &program, uint32_t rule);

// True when every rule's vertices already sit in the columns of the same index.
// A programme for which this is false is one the GPU rewrite would read wrongly.
bool ruleColumnsAreIdentity(const compiler::CompiledProgram &program);

// Every row the rule matched, with `index` set to its table row so the dice
// roll is the same one the GPU would make.
// `keep(row)` decides which table rows become MatchedRows at all - the
// caller can roll the dice here, so a per-pair rule at one in eight hundred
// does not turn millions of declined pairs into heap objects every tick.
using RowFilter = std::function<bool(uint32_t row)>;

std::vector<MatchedRow> gatherRuleRows(const compiler::CompiledProgram &program, const CpuProgramRunner &runner, uint32_t rule,
                                       const RowFilter &keep = RowFilter());

} // namespace graphrewrite::runtime
