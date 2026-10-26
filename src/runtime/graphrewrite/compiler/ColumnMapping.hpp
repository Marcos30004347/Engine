#pragma once

// Translates the merged graph's columns into each rule's own table columns, and
// allocates the provenance columns the rewrite phase reads.

#include "graphrewrite/compiler/PatternMerger.hpp"
#include "graphrewrite/compiler/Program.hpp"

namespace graphrewrite::compiler {

bool assignRuleColumns(const std::vector<MergedGraph> &threads, const std::vector<RuleGraph> &graphs, CompiledProgram &program,
                       DiagnosticSink &sink);

} // namespace graphrewrite::compiler
