#include "WorldPipelineEditSession.hpp"
#include "WorldMapPatchAdapter.hpp"
#include "WorldPipelineStages.hpp"
namespace procedural::world
{
namespace
{
rewrite::PatchGenerationFactory factory(const WorldPipelineEditSession &session, const WorldPipelineCallbacks &callbacks)
{
  return [&](const rewrite::EditablePatchSet &geometry, rewrite::PatchGenerationBuild &build, std::string *error)
  {
    using namespace rewrite;
    auto partition = session.partition;
    for (auto &[id, patch] : geometry.patches)
    {
      if (id == 0 || id > partition.graph.nodeCount())
      {
        if (error)
          *error = "editable source identity no longer maps to world hierarchy";
        return false;
      }
      auto &node = partition.graph.node(uint32_t(id - 1));
      node.area.clear();
      for (auto vertex : patch.vertices)
      {
        auto p = geometry.vertices.at(vertex);
        node.area.push_back({p.x, p.z});
      }
    }
    if (!partition.graph.validate(error))
      return false;
    build.tasks = worldPipelineGenerators(callbacks);
    GenerationKey source{"world.partition-override", 1}, output{"world.partition", 1};
    auto &root = build.tasks.front();
    root.descriptor.reads = {{source, typeid(GeneratedWorld), 1}};
    root.generate = [=](const GenerationRequest &request, GenerationSnapshot &delta, std::string *)
    {
      auto p = request.context.get<GeneratedWorld>(source);
      if (!p)
        return false;
      delta.emplace(output, GenerationProduct{*p, 1});
      return true;
    };
    build.inputs = {{{"world.settings", 1}, {session.settings, 1, 1}}, {source, {std::move(partition), 1, geometry.revision}}, {{"world.assignments", 1}, {geometry, 1, geometry.revision}}};
    return true;
  };
}
} // namespace
const GeneratedWorld *WorldPipelineEditSession::world() const
{
  auto it = state.generation.products.find({"world.summary", 1});
  return it == state.generation.products.end() ? nullptr : std::any_cast<GeneratedWorld>(&it->second.value);
}
bool beginWorldPipelineEditing(const WorldPipelineSettings &settings, WorldPipelineEditSession &out, const WorldPipelineCallbacks &callbacks, std::string *error)
{
  WorldPipelineEditSession candidate;
  candidate.settings = settings;
  if (!executeWorldPipelineStage(0, settings, candidate.partition, callbacks, error))
    return false;
  if (!rewrite::importWorldPatches(candidate.partition.graph, candidate.state.geometry, error))
    return false;
  // Partition precedes land/water assignment; imported protection flags from the
  // pre-landmass graph are not authoritative land-use restrictions.
  for (auto &[id, p] : candidate.state.geometry.patches)
    p.use = rewrite::PatchLandUse::Unassigned;
  rewrite::GenerationUpdateReport report;
  if (!rewrite::editGeneratedWorld({}, candidate.state.geometry.revision, 0, settings.brief.seed, factory(candidate, callbacks), candidate.state, report, {}, callbacks.cancelled, error))
    return false;
  const auto *generated = candidate.world();
  for (auto &[id, patch] : candidate.state.geometry.patches)
  {
    const auto &node = generated->graph.node(uint32_t(id - 1));
    patch.owner = node.region == kInvalidNode ? 0 : uint64_t(node.region) + 1;
    patch.biome = node.biome;
    patch.use = node.land ? rewrite::PatchLandUse::Unassigned : rewrite::PatchLandUse::Protected;
  }
  // Assignment metadata is published with the initial snapshot; later edits
  // preserve it even if a downstream descriptive region solver is rerun.
  candidate.state.generation.inputs.at({"world.assignments", 1}).value = candidate.state.geometry;
  candidate.state.generation.products.at({"world.assignments", 1}).value = candidate.state.geometry;
  out = std::move(candidate);
  return true;
}
bool editWorldPipeline(
    const rewrite::PatchGenerationEdit &edit,
    uint64_t expectedGeometry,
    uint64_t expectedGeneration,
    WorldPipelineEditSession &session,
    rewrite::GenerationUpdateReport &report,
    const WorldPipelineCallbacks &callbacks,
    std::string *error)
{
  return rewrite::editGeneratedWorld(edit, expectedGeometry, expectedGeneration, session.settings.brief.seed, factory(session, callbacks), session.state, report, {}, callbacks.cancelled, error);
}
} // namespace procedural::world
