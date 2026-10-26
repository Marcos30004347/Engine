#pragma once

#include "graphrewrite/compiler/PatternMerger.hpp"
#include "graphrewrite/compiler/Program.hpp"

namespace graphrewrite::compiler {

// Emits one instruction per merged edge, plus the bookkeeping instructions.
//
// The invariant that matters: EVERY merged edge is emitted EXACTLY ONCE. A
// dropped edge leaves a rule silently unmatched - the programme still verifies,
// still runs, and simply produces nothing for that rule.
// `edgeCounts`, when non-empty, is how many edges of each type the graph holds.
// The planner then starts every rule from its RAREST edge and prefers rare edges
// at each step, which is what keeps the intermediate tables small.
bool buildProgram(const std::vector<MergedGraph> &threads, const std::vector<RuleGraph> &graphs, EdgeTypeTable table,
                  DiagnosticSink &sink, CompiledProgram &out, const std::vector<uint32_t> &edgeCounts = {});

} // namespace graphrewrite::compiler
