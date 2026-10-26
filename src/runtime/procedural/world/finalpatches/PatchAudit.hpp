#pragma once

#include "procedural/world/WorldFinalPatches.hpp"

#include <memory>
#include <vector>

namespace procedural::world::finalpatches {

using rewrite::WorldFinalPatches;

// The frozen-patch audit as one class per question.
//
// auditWorldFinalPatches was a 187-line function running eight unrelated
// checks in sequence. Each check is a separate claim about the product, and
// each had its own vocabulary of local maps and sets that stayed in scope for
// the rest of the function whether the later checks wanted them or not.
//
// As classes each check states one claim, owns only what it needs, names
// itself in the failure, and can be run - or skipped - on its own.

// One claim about a frozen patch set.
//
// A check reports a *fault*: an empty string means the claim holds. It never
// mutates the product, so the audit is equally valid on a freshly built set
// and on one deserialised from disk before publishing it.
class AuditCheck
{
public:
  virtual ~AuditCheck() = default;

  // Stable name, reported with the fault so a failure says which claim broke.
  virtual const char *name() const = 0;

  virtual std::string fault(const WorldFinalPatches &) const = 0;
};

using CheckPtr = std::shared_ptr<AuditCheck>;

// The tile partition is described by cut lines. They must be finite, strictly
// inside the map bounds and strictly increasing - a repeated or out-of-order
// cut would make a tile of no area or of negative extent.
class TileCutsCheck final : public AuditCheck
{
public:
  const char *name() const override
  {
    return "tile-cuts";
  }

  std::string fault(const WorldFinalPatches &) const override;
};

// Faces are identified by their position in the list, so face i must carry id
// i + 1; every face needs at least three vertices, a plane that is finite, and
// a role and layer in range. Edge identities must be unique and their
// endpoints and faces in range.
class TopologyCheck final : public AuditCheck
{
public:
  const char *name() const override
  {
    return "topology";
  }

  std::string fault(const WorldFinalPatches &) const override;
};

// The mesh may add vertices - the refinement stations edges and seeds
// interiors - but it must not move the ones the arrangement froze, because
// every consumer indexes them positionally.
class FrozenVertexCheck final : public AuditCheck
{
public:
  const char *name() const override
  {
    return "frozen-vertices";
  }

  std::string fault(const WorldFinalPatches &) const override;
};

// Every published triangle belongs to a face and carries that face's material,
// layer and role - the mesh may not relabel ground as road. Every triangle is
// wound positively, and the layer-zero triangles cover the map rectangle
// exactly: a gap or an overlap in ground coverage is a hole in the world.
class PublishedTriangleCheck final : public AuditCheck
{
public:
  const char *name() const override
  {
    return "published-triangles";
  }

  std::string fault(const WorldFinalPatches &) const override;
};

// Each canonical edge must survive as an edge of the published mesh on both
// sides it borders. A triangulation is free to add edges; it is not free to
// cut across one that owns a material boundary.
class CanonicalEdgeCheck final : public AuditCheck
{
public:
  const char *name() const override
  {
    return "canonical-edges";
  }

  std::string fault(const WorldFinalPatches &) const override;
};

// The same claim one level down, inside each patch's own triangulation: every
// constraint handed to the CDT comes back as a triangle edge, with its
// endpoints still on the canonical vertices rather than near them, and no
// canonical edge is missing from a face that borders it.
class ConstraintFidelityCheck final : public AuditCheck
{
public:
  const char *name() const override
  {
    return "cdt-constraints";
  }

  std::string fault(const WorldFinalPatches &) const override;
};

// Each producer's contour segment must be covered by arrangement edges that
// carry its provenance, contiguously and without overlap, over its whole
// length. This is what makes "who owns this ground" answerable after the
// arrangement has cut every input against every other.
class SourceChainCheck final : public AuditCheck
{
public:
  const char *name() const override
  {
    return "source-chains";
  }

  std::string fault(const WorldFinalPatches &) const override;
};

// Tiles partition the mesh: every face belongs to exactly one tile, every
// triangle is listed exactly once, no triangle's vertices cross its tile's
// rectangle, and each tile's seam manifest is exactly the edges whose two
// faces fall in different tiles. The manifest is what lets a tile be rebuilt
// without its neighbours.
class TilePartitionCheck final : public AuditCheck
{
public:
  const char *name() const override
  {
    return "tile-partition";
  }

  std::string fault(const WorldFinalPatches &) const override;
};

// The checks, in the order they run. Ordered cheapest-and-most-fundamental
// first, so a broken product fails on the claim nearest its cause.
const std::vector<CheckPtr> &worldPatchChecks();

} // namespace procedural::world::finalpatches
