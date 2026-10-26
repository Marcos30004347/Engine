#pragma once

// A versioned binary form of a compiled programme.
//
// This is the contract between the compiler and the runtime, so the edge type
// table travels with it: the runtime's page partitions are keyed by the ids
// assigned here, and reading them against a different table would silently
// match the wrong edges.

#include "graphrewrite/compiler/Program.hpp"
#include "graphrewrite/compiler/UsageValues.hpp"

#include <cstdint>
#include <vector>

namespace graphrewrite::compiler {

constexpr uint32_t programMagic = 0x50575247u; // "GRWP"
constexpr uint32_t programVersion = 1u;

bool writeProgram(const CompiledProgram &program, const UsageValues &usage, std::vector<uint8_t> &out);

bool readProgram(const uint8_t *data, size_t size, CompiledProgram &program, UsageValues &usage, DiagnosticSink &sink);

} // namespace graphrewrite::compiler
