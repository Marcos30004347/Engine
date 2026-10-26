#pragma once

// Structural checks on a compiled programme.
//
// These are the invariants the runtime is allowed to assume. Several of them
// describe failures that are otherwise SILENT - a dropped merged edge, a rule
// never reported - where the programme still runs and simply produces nothing.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/compiler/UsageValues.hpp"

namespace graphrewrite::compiler {

bool verifyProgram(const CompiledProgram &program, const UsageValues &usage, DiagnosticSink &sink);

} // namespace graphrewrite::compiler
