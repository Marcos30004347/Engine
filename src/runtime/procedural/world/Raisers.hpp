#pragma once

#include "procedural/world/MapGraph.hpp"

#include <string>

namespace procedural::world {

struct RaiseSettings
{
  // Fraction of cells that should end up land. Honoured exactly rather than
  // approached by threshold tuning - see raiseLand.
  double landFraction = 0.35;

  // Coastline irregularity, 0..1. Zero gives the archetype's clean analytic
  // shape; higher values warp it so the coast is not obviously a circle or a
  // half-plane.
  double coastComplexity = 0.5;

  // How much of that irregularity is fine structure rather than broad
  // wandering, 0..1. The warp is multi-octave, and this weights the finer
  // octaves: low gives smooth continental bends, high gives peninsulas,
  // inlets, headlands and offshore islets.
  double coastDetail = 0.55;

  // Number of landmass cores the shape archetypes are built from.
  //
  // One core is a disc, and a disc is what made every island read as a blob
  // however much noise was thrown at its edge. Several overlapping cores at
  // layout-chosen positions give a lobed landmass with peninsulas, gulfs and
  // outlying islands - which is a *shape* decision, not a noise one, and is why
  // it lives here rather than in the warp.
  uint32_t landmassCores = 4u;

  // Metres. Needed because the archetype fields are defined against the world's
  // own size rather than in normalized units.
  double extentMetres = 361000.0;

  uint64_t seed = 0u;
};

// Classifies every cell in the graph as land or water according to the brief's
// shape tag, then derives the coastal and shallow flags.
//
// The archetype decides the *shape* of the land; landFraction decides *how
// much*. Keeping those independent is what lets "archipelago" and "60% land"
// both be honoured, which a fixed field threshold cannot do: the same threshold
// produces wildly different land fractions as the noise or the cell count
// changes.
bool raiseLand(Tag shape, const RaiseSettings &settings, MapGraph &graph, std::string *error = nullptr);

// Measures the land fraction of cells touching the world border. This is the
// classifier the reference generator uses to name a map's archetype, and it is
// used here in reverse: generate, measure, and confirm the result matches what
// was asked for.
double borderLandFraction(const MapGraph &graph, double extentMetres);

// Names the archetype a finished map actually looks like, independent of what
// was requested. Disagreement with the requested tag is a generator bug.
Tag measuredShape(const MapGraph &graph, double extentMetres);

} // namespace procedural::world
