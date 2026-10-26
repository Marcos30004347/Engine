#pragma once

// The whole front-to-back pipeline, in one call.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/compiler/UsageValues.hpp"
#include "graphrewrite/frontend/OrExpansion.hpp"

#include <string>
#include <vector>

namespace graphrewrite::compiler {

struct CompileOptions
{
  OrExpansionOptions expansion;
  bool verify = true;

  // Edges per type, indexed by declaration order. Supplying these makes the
  // planner start each rule from its rarest edge; leaving it empty falls back to
  // ordering by how many rules an edge is shared with.
  std::vector<uint32_t> edgeCounts;
};

// parse -> analyse (expanding `or`) -> edge types -> rule graphs -> join ->
// emit -> per-rule columns -> usage values -> verify
bool compile(const std::string &source, const std::string &path, const CompileOptions &options, DiagnosticSink &sink,
             CompiledProgram &program, UsageValues &usage);

} // namespace graphrewrite::compiler
