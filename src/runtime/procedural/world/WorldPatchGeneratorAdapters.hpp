#pragma once
#include "WorldDwellingPlan.hpp"
#include "WorldFarmFields.hpp"
#include "WorldGeneratorDirector.hpp"

namespace procedural::world::rewrite {
struct FarmGenerationBrief
{
  FarmFieldInput geometry;
  FarmFieldSettings settings;
};

// Domain-specific native algorithms behind the common protocol. Briefs retain
// authored IDs/seeds: adapter scheduling never changes geometry randomness.
GenerationTask farmFieldsGenerator(uint64_t domain);
GenerationTask dwellingPlanGenerator(uint64_t domain);
GenerationTask dwellingMeshGenerator(uint64_t domain);
} // namespace procedural::world::rewrite
