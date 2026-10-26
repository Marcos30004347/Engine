#pragma once

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"
#include "graphrewrite/runtime/IProgramRunner.hpp"

namespace graphrewrite::runtime {

// Shared by both runners: which edge type each rule is seeded from, what that
// costs against the graph as it stands, and what the cheapest seed would be.
PlanCheck checkProgramPlan(const compiler::CompiledProgram &program, const EdgePageStore &store);

} // namespace graphrewrite::runtime
