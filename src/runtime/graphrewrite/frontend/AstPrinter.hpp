#pragma once

// Two renderings of an AST, for two different jobs.
//
//   printAst  - source text that re-parses to the same tree. Used to check that
//               the parser did not quietly drop something.
//   dumpAst   - a canonical structural form. Used to COMPARE two trees, because
//               comparing printed source cannot tell `A or B, C` from
//               `A, B or C` if the printer happens to bracket them the same way.

#include "graphrewrite/frontend/Ast.hpp"

#include <string>

namespace graphrewrite {

std::string printAst(const Program &program);
std::string dumpAst(const Program &program);

} // namespace graphrewrite
