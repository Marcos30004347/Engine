#pragma once
#include "WorldGeneratorDirector.hpp"
#include "WorldPipeline.hpp"

namespace procedural::world {
// All active map stages exposed through typed dependencies. Terrain/network
// kernels remain World-scoped until locality can be proven independently.
std::vector<rewrite::GenerationTask> worldPipelineGenerators(const WorldPipelineCallbacks & = {});
bool generateWorldDirected(const WorldPipelineSettings &, GeneratedWorld &, const WorldPipelineCallbacks & = {}, std::string *error = nullptr);
} // namespace procedural::world
