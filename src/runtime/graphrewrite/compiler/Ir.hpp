#pragma once

// Lowering from the analysed AST to the compiler's own representation.

#include "graphrewrite/compiler/EdgeTypeTable.hpp"
#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/frontend/SemanticAnalysis.hpp"

#include <string>
#include <vector>

namespace graphrewrite::compiler {

// Builds the edge type table from the `let` declarations, in declaration order.
bool buildEdgeTypeTable(const AnalysedProgram &analysed, EdgeTypeTable &out, DiagnosticSink &sink);

// A readable dump, used as a golden file in the tests.
std::string dumpEdgeTypeTable(const EdgeTypeTable &table);

} // namespace graphrewrite::compiler
