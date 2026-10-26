#pragma once
#include "WorldPipeline.hpp"

namespace procedural::world {
// Native stage kernels shared by direct compatibility runner and DAG adapters.
// Mutation is confined to the caller's private candidate; stage order is required.
const std::vector<std::string> &worldPipelineStageNames();
bool executeWorldPipelineStage(size_t, const WorldPipelineSettings &, GeneratedWorld &, const WorldPipelineCallbacks &, std::string *error = nullptr);
} // namespace procedural::world
