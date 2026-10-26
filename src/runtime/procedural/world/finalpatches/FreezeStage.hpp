#pragma once

#include "procedural/world/finalpatches/PatchAudit.hpp"

#include <functional>

namespace procedural::world::finalpatches {

using rewrite::SurfaceMapSettings;
using rewrite::SurfaceRefinement;
using rewrite::WorldContourBatch;

// Freezing the world's patches, as one class per step.
//
// freezeWorldPatches was a 109-line function doing eight things in sequence
// with no boundary between them: validating producers, normalising cuts,
// renumbering contours, arranging, stationing, meshing, linking provenance and
// tiling. Read from the top it is a wall; read as classes it is a pipeline.

// The state the pipeline carries.
//
// `settings` is deliberately a copy: the normalisation step sorts and
// deduplicates the tile cuts, and the caller's copy must not change under it.
struct FreezeBuild
{
  std::vector<WorldContourBatch> batches;
  SurfaceMapSettings settings;
  const SurfaceRefinement &refinement;
  const std::function<double(double, double)> &terrain;
  uint64_t revision = 0;

  // The product under construction.
  WorldFinalPatches result;

  // Contours renumbered into one global sequence; what the arrangement is
  // actually built from.
  std::vector<rewrite::SurfaceContour> contours;
};

// One step of the freeze.
class FreezeStage
{
public:
  virtual ~FreezeStage() = default;

  virtual const char *name() const = 0;

  virtual bool run(FreezeBuild &, std::string *error) = 0;
};

using FreezeStagePtr = std::shared_ptr<FreezeStage>;

// Rejects an oversized or ambiguous producer set, then orders the batches by
// priority and producer.
//
// The order is the ownership order: a later batch's contours win. Sorting by
// producer as the tie-break is what keeps the result independent of the order
// the batches happened to arrive in, which on a threaded producer is the order
// they happened to finish in.
class OrderBatchesStage final : public FreezeStage
{
public:
  const char *name() const override
  {
    return "order-batches";
  }

  bool run(FreezeBuild &, std::string *error) override;
};

// Sorts and deduplicates the tile cut lines.
//
// Globally, not per batch: two producers asking for the same cut must not
// produce two tiles of zero width between them.
class NormaliseCutsStage final : public FreezeStage
{
public:
  const char *name() const override
  {
    return "normalise-cuts";
  }

  bool run(FreezeBuild &, std::string *error) override;
};

// Renumbers every contour into one global sequence and records, per segment,
// the producer and contour it came from.
//
// This is where provenance is created. After the arrangement has cut every
// input against every other, these chains are the only way back to "who asked
// for this ground".
class CollectContoursStage final : public FreezeStage
{
public:
  const char *name() const override
  {
    return "collect-contours";
  }

  bool run(FreezeBuild &, std::string *error) override;
};

// Resolves every contour intersection into one planar arrangement, stations
// its edges against the terrain, and triangulates each face.
//
// Three kernel calls with nothing between them, so they are one step: any
// split would only be a place to put a variable.
class ArrangeAndMeshStage final : public FreezeStage
{
public:
  const char *name() const override
  {
    return "arrange-and-mesh";
  }

  bool run(FreezeBuild &, std::string *error) override;
};

// Walks each source segment along the arrangement edges that carry its
// provenance, in order along the segment, so the chain reads from one end to
// the other. The audit's source-chain check is the proof that this covered it.
class LinkProvenanceStage final : public FreezeStage
{
public:
  const char *name() const override
  {
    return "link-provenance";
  }

  bool run(FreezeBuild &, std::string *error) override;
};

// Assigns each face to the tile containing its centre, lists each tile's
// triangles, and records the seam edges between tiles.
//
// A face goes to the tile its centre falls in rather than being split, because
// splitting it would create geometry no producer authored.
class TileStage final : public FreezeStage
{
public:
  const char *name() const override
  {
    return "tile";
  }

  bool run(FreezeBuild &, std::string *error) override;
};

// The pipeline, in order.
const std::vector<FreezeStagePtr> &freezePipeline();

// Split arrangement edges beside terrain-following faces wherever a straight
// edge would depart from the terrain by more than the tolerance.
//
// The frozen map carries its own stations on generated cuts and support
// scanlines, which no producer can supply. Graph edits keep both face rings
// consistent.
bool stationArrangementEdges(rewrite::SurfaceArrangement &, const std::function<double(double, double)> &terrain, const SurfaceRefinement &, std::string *error);

} // namespace procedural::world::finalpatches
