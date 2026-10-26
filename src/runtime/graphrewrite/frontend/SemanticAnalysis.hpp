#pragma once

// Checks that hold across declarations, run after `or` has been expanded away.
//
// Order matters: expansion happens FIRST, and then every variant is analysed on
// its own. A variable bound by one alternative need not be bound by another, so
// asking the question of the un-expanded rule gets the wrong answer:
//
//   rule r: when A likes B or A likes D then A likes B.
//
// is fine for r#0 and an unbound `B` for r#1, and the diagnostic has to say
// which.

#include "graphrewrite/frontend/Ast.hpp"
#include "graphrewrite/frontend/Diagnostics.hpp"
#include "graphrewrite/frontend/OrExpansion.hpp"

#include <vector>

namespace graphrewrite {

struct AnalysedProgram
{
  std::vector<const LetDecl *> declarations; // edge types, in declaration order
  std::vector<RuleVariant> variants;         // AND-only
};

// Returns false if any error was reported. Always runs every check: one pass
// reports everything it can find, rather than stopping at the first problem.
bool analyse(const Program &program, const OrExpansionOptions &options, DiagnosticSink &sink, AnalysedProgram &out);

} // namespace graphrewrite
