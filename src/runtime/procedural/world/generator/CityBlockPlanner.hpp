#pragma once

#include "Generator.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/WorldCityMasterPlan.hpp"
#include "procedural/world/WorldCityPlaza.hpp"

#include <cstdint>
#include <vector>

namespace procedural::world::generator {

using rewrite::DistrictUse;
using rewrite::Position;

// What a block is made of, decided for the block as a whole.
//
// Lots used to be cut from a block and then each one asked, on its own, what
// it should be - so a block was a bag of unrelated plots and nothing in it
// belonged to a neighbourhood. A block knows things a lot cannot: which of
// its edges face a street and which face the backs of other houses, where the
// way into its middle should go, that the corner plots are worth more as
// trade than as another cottage, and that what is left in the centre is a
// garden rather than a leftover.
//
// The parts below are that knowledge, as geometry.
enum class CityBlockLotRole : uint8_t
{
  Dwelling,   // A house on the frontage.
  ShopHouse,  // Trade on the ground floor, living above: the corners and the best frontage.
  Workshop,   // A trade that needs a yard rather than a shopfront.
  Passage,    // Carries the covered way into the block through its ground floor.
  // What a neighbourhood needs rather than what a plot is worth. A street of
  // houses with nowhere to buy bread is a street nobody lived on; these are
  // placed because the block needs them, and only then does the plot that
  // carries one stop being a dwelling.
  Pub,
  Inn,
  Bakehouse,
  Smithy,
  // The countryside end of the town. A croft is not a small version of an
  // urban plot: the cottage sits on the street end of a long strip, the yard
  // and the pen are behind it, and the rest is grown on. Medieval English
  // toft and croft.
  Cottage
};

// What a block owes its neighbourhood, by how many households it holds.
struct CityBlockNeeds
{
  uint32_t dwellingsPerPub = 14;
  uint32_t dwellingsPerBakehouse = 22;
  bool smithyInCraftBlocks = true;
  bool innOnThroughStreets = true; // A block with two or more street edges may take one.
};

struct CityBlockLot
{
  std::vector<Position> boundary;
  // A croft's parts, in the order they run back from the street: the toft the
  // cottage stands on, the yard it works in, the pen, and the garden strip.
  // Empty on an urban plot.
  std::vector<Position> toft, yard, pen, garden;
  CityBlockLotRole role = CityBlockLotRole::Dwelling;
  uint32_t frontageEdge = 0; // Which street edge it fronts.
  double area = 0;
  bool corner = false;
  bool courtFacing = false; // This lot opens onto the shared paved interior.
  // The lots either side along the same frontage, or UINT32_MAX. A terrace is
  // built from these: neighbours share a party line and meet along it.
  uint32_t before = UINT32_MAX, after = UINT32_MAX;
};

struct CityBlockPlanInput
{
  std::vector<Position> boundary;   // The block, counter clockwise.
  std::vector<bool> streetEdges;    // Per boundary edge: does a street run along it.
  // Per boundary edge, the direction of the street that fronts it, as a unit
  // vector; zero where there is none. Lots are cut square to THIS and not to
  // the block edge: a block edge is within a few degrees of its street, and
  // those few degrees are the difference between a packed frontage and a row
  // of lots that each meet the road at their own angle.
  std::vector<Position> streetAxis;
  DistrictUse use = DistrictUse::Residential;
  uint64_t seed = 7, id = 1;

  double sidewalkMetres = 1.4;   // Paved strip between the street and the plots.
  double corridorMetres = 1.6;   // The way into the middle of the block.
  double lotDepthMetres = 11.;   // How far back a frontage plot runs.
  double lotFrontageMetres = 6.; // How wide, before the block's own width is shared out.
  double minimumLotMetres2 = 45.;
  double minimumLotWidthMetres = 3.5; // Structural module plus both lot setbacks.
  double gardenMinimumMetres2 = 40.; // Below this the middle is not worth keeping open.
  double maximumCourtMetres2 = 240.; // Deep interiors receive additional lane-fronted lots.
  CityBlockNeeds needs;

  // A croft, when the block is a suburb. The strip runs back from the street:
  // cottage, then the ground it works.
  // The block is a public square: paved through, no plots, and whatever
  // stands on it stands in the open. A plaza is not a block with the houses
  // left out - the ground is the thing, and it is paved to its edges.
  bool plaza = false;
  bool conformingQuadLots = true; // Bilinear subdivision between opposing block edges.
  bool commonCourt = true; // Reserve a central square and approaches before lots.
  bool inwardCourt = false; // Keep a generous court and aim adjacent dwellings inward.
  // What stands on a square.
  //
  // Not a scatter of little huts: a market square's buildings are shambles -
  // long narrow permanent stalls built in a glued run along one side, a
  // butcher or a baker to each bay - and a market hall, which is the biggest
  // roof in the town after the church. Sizing them like cottages is what made
  // the square read as a field with sheds in it.
  double shamblesFrontageMetres = 3.8;  // One bay of a shambles run.
  double shamblesDepthMetres = 7.5;
  double marketHallWidthMetres = 15.;
  double marketHallDepthMetres = 9.5;
  bool marketHall = true;
  bool crofts = false;
  double croftFrontageMetres = 11.;  // Wider than a town plot: a croft is not a terrace.
  double toftDepthMetres = 9.;       // The built end.
  double yardDepthMetres = 7.;       // Working ground: the byre, the midden, the store.
  double penDepthMetres = 6.;        // Stock.
};

struct CityBlockPlan
{
  uint64_t id = 0;
  std::vector<std::vector<Position>> sidewalks; // One strip per street edge.
  std::vector<CityBlockLot> lots;
  std::vector<std::vector<Position>> corridors; // Ways from the street into the middle.
  std::vector<std::vector<Position>> gardens;   // What the middle is left as.
  std::vector<std::vector<Position>> paving;    // A plaza's open ground.
  std::vector<std::vector<Position>> stalls;    // Shambles bays, in glued runs.
  std::vector<std::vector<Position>> halls;     // The market hall, if the square is big enough.
  // A plaza's structures: the arcade round it, the gates where the streets
  // come in, the dais and the monument. The stalls and halls above are the
  // same rings the plan holds, kept here so a block reads like any other.
  rewrite::CityPlazaPlan plaza;
  std::vector<std::string> notes;
  uint32_t dwellings = 0, trades = 0;
};

// Plans one block. Pure: the same input gives the same block.
bool planCityBlock(const CityBlockPlanInput &, CityBlockPlan &, std::string *error = nullptr);

const char *nameOf(CityBlockLotRole);

} // namespace procedural::world::generator
