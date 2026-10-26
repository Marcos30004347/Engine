#pragma once
#include "WorldGenerationEdits.hpp"
#include "WorldPipelineDirector.hpp"

namespace procedural::world {
struct WorldPipelineEditSession
{
  WorldPipelineSettings settings;
  GeneratedWorld partition;
  rewrite::EditableGenerationWorld state;
  const GeneratedWorld *world() const;
};

// Bounded editable domains use the same stage DAG and native world products as
// the application. Whole-world network stages invalidate conservatively.
bool beginWorldPipelineEditing(const WorldPipelineSettings &, WorldPipelineEditSession &, const WorldPipelineCallbacks & = {}, std::string *error = nullptr);
bool editWorldPipeline(
    const rewrite::PatchGenerationEdit &,
    uint64_t expectedGeometry,
    uint64_t expectedGeneration,
    WorldPipelineEditSession &,
    rewrite::GenerationUpdateReport &,
    const WorldPipelineCallbacks & = {},
    std::string *error = nullptr);
} // namespace procedural::world
