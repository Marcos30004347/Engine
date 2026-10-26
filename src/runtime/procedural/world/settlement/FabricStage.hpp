#pragma once

#include "procedural/world/WorldSettlementFabricAdapter.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace procedural::world::settlement {

using rewrite::CitySettings;
using rewrite::CityTerritory;
using rewrite::SettlementFabric;
using rewrite::SettlementFabricSettings;
using rewrite::WorldSettlementSite;

// The settlement fabric as small stage classes.
//
// composeSettlementFabric is one 2,054-line function. Unlike the world map,
// its phases are genuinely entangled - twenty-five shared locals and seven
// lambdas capturing them by reference - so it cannot be split in one move
// without inventing the shared state those lambdas stand for.
//
// It is being split from the tail forward, because the tail is already
// separable: the phases below touch nothing the earlier ones left on the
// stack, only the fabric being built and a handful of settings. Each phase
// that moves out takes its shared state with it into `FabricBuild`, so the
// remainder shrinks and the next phase becomes separable in turn.

// The state a stage reads and writes.
//
// This is deliberately explicit rather than a bag of references captured from
// an enclosing scope: what a stage can touch is what appears here, which is
// the difference between a phase and a paragraph.
struct FabricBuild
{
  const WorldSettlementSite &site;
  const CityTerritory &territory;
  const SettlementFabricSettings &settings;

  // The product under construction. Stages mutate it; the caller owns a
  // private candidate, so a failure discards it rather than publishing half a
  // settlement.
  SettlementFabric &fabric;

  // Layout settings after the class grammar has adjusted them. Several stages
  // need the street widths and setbacks the layout actually used, not the ones
  // the caller asked for.
  CitySettings city;

  // The civic anchor the gate roads converge on. Zero when the settlement has
  // no plaza.
  uint64_t plazaCell = 0;

  // Accepted jetty projection per house, in metres.
  //
  // Shared state, found by moving the phases rather than by reading them: the
  // population stage decides each projection, and the neighbourhood ledger
  // counts them a stage later. It lives here because that is what "shared
  // between two phases" means once the phases are objects - on the stack of
  // one enormous function it was invisible.
  std::map<uint64_t, double> acceptedJetty;
};

// One phase of building a settlement's fabric.
//
// A stage advances the build in place and reports a hard failure through
// `error`. It does *not* fail for ordinary shortfalls: a district with no room
// for its compound, a house too small to plan, a programme the population
// could not fill are recorded on `fabric.rejections` and the settlement still
// stands. A settlement that cannot be finished perfectly is still a
// settlement; only a broken invariant is an error.
class FabricStage
{
public:
  virtual ~FabricStage() = default;

  // Stable identity, used in ledger entries and in the stage list.
  virtual const char *name() const = 0;

  virtual bool run(FabricBuild &, std::string *error) = 0;
};

using StagePtr = std::shared_ptr<FabricStage>;

// Fits residents to the layout's plots and plans what each house contains.
//
// Runs the settlement director over the accepted plots: households, tenancy,
// employment and services, with explicit terrain and water eligibility so a
// house is never fitted where nobody could live. Then decides jettying per
// house - whether an upper floor may project over the street - which needs the
// street width in front of that house and what the facade opposite already
// projects, so it cannot be decided before the layout exists.
class PopulationStage final : public FabricStage
{
public:
  const char *name() const override
  {
    return "population";
  }

  bool run(FabricBuild &, std::string *error) override;
};

// Places the settlement's institutional structures.
//
// Every accepted non-housing district except the plaza gets a compound -
// castle, manor, church or civic hall - planned inside the layout's own
// compound inset and approached from the district's boundary portal. Each
// inhabited wing is a dwelling brief planned by the dwelling planner, so rooms,
// doors and stairs come from the same rules as an ordinary house.
class CompoundStage final : public FabricStage
{
public:
  const char *name() const override
  {
    return "compounds";
  }

  bool run(FabricBuild &, std::string *error) override;
};

// Measures each district from what the director actually placed in it.
//
// Houses, residents, workers, shops and jettied facades per accepted district.
// Measured rather than authored: the neighbourhood is the district, so its
// ledger is a count of what is there and not a target someone set.
class NeighbourhoodStage final : public FabricStage
{
public:
  const char *name() const override
  {
    return "neighbourhoods";
  }

  bool run(FabricBuild &, std::string *error) override;
};

// Turns the plan into geometry.
//
// City massing is meshed on the terrace, farm buildings on the real ground -
// they are different surfaces on purpose, because the terrace is what the
// streets and houses were levelled onto and the fields were not. Produces the
// instance triangles and the structural mesh the runtime builder consumes.
class InstanceStage final : public FabricStage
{
public:
  const char *name() const override
  {
    return "instances";
  }

  bool run(FabricBuild &, std::string *error) override;
};

// The stages that have been extracted so far, in order.
//
// composeSettlementFabric runs its remaining inline phases and then hands the
// build to these. As more phases move out the list grows upward, until the
// function is the orchestrator and nothing else.
const std::vector<StagePtr> &fabricTailStages();

} // namespace procedural::world::settlement
