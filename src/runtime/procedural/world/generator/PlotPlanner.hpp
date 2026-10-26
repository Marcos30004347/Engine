#pragma once

#include "ComplexDwellingPlanner.hpp"
#include "procedural/world/WorldDwellingPlan.hpp"

#include <string>
#include <utility>
#include <vector>

namespace procedural::world::generator {

// What a lot does with its ground.
//
// The buildings step used to be one rule - inset the lot, keep the piece - with
// a coverage cap bolted on, then a yard, then a fence, then outbuildings, each
// as a special case of the last. What it never had was the thing it was
// standing in for: a description of the plot as a *plot*. A slum lot and a
// patrician lot are not a small house and a large house on the same rule,
// they are different uses of ground - one built to its boundary, the other
// mostly not built on at all - and how much of the lot the house alone takes
// is the property that separates them.
//
// So this is the layer that decides that. It takes a lot, which edge of it is
// the street, and a standing, and returns how the ground is used: where the
// house stands and how many storeys it carries, what stands in the yard, what
// is garden and what is working ground, where the fence runs. The house's
// exterior is composed here; its interior is the next layer's problem, and the
// render is the one after that.
//
// Watabou's town does this by ward: every ward class carries its own minSq,
// gridChaos, sizeChaos and emptyProb, and a Farm places "housing" as a 4x4
// somewhere inside its patch rather than filling it. The standings below are
// those wards read as plots.
enum class PlotStanding : uint8_t
{
  Slum,        // Built over; one or two storeys; nothing else on the ground.
  Modest,      // A craftsman's plot: most of it house, a yard behind.
  Comfortable, // A merchant's: two storeys, a garden, one outbuilding.
  Wealthy,     // A patrician's: a house with wings on a third of the plot, grounds, outbuildings.
  Grand,       // A mansion on a quarter of it, the rest laid out.
  Rural        // A cottage on a large plot that is mostly a fenced pen with a barn.
};

struct PlotBrief
{
  uint64_t id = 0, seed = 0;

  // The ground, counter clockwise; y ignored.
  std::vector<rewrite::Position> lot;

  // Ground inside the outline that is not the lot's: the court of a
  // perimeter block. A ring lot's region carries it as a hole, and planned on
  // the outline alone the house's back wing stood in the court.
  std::vector<std::vector<rewrite::Position>> holes;

  // The edges of the lot that face a street, as segments. Empty means the
  // planner takes the lot's longest edge for the street, because a lot drawn
  // on its own is a lot on a lane.
  std::vector<std::pair<rewrite::Position, rewrite::Position>> frontage;

  PlotStanding standing = PlotStanding::Modest;

  // Share of the lot the dwelling alone covers. This is the number the
  // standing is really about.
  double houseShare = .5;

  // Of the ground the house leaves, the share behind it kept as garden rather
  // than working yard. Zero for a plot that has no garden.
  double gardenShare = 0;

  uint32_t minimumStoreys = 1, maximumStoreys = 2;
  uint32_t outbuildings = 0;

  // A paddock: the open ground is fenced and holds a barn.
  bool fenced = false;

  // How far the house stands from every boundary it does not front, and from
  // the street.
  double setbackMetres = 1.0;
  double frontSetbackMetres = 0;

  double storeyMetres = 2.9;
  double foundationMetres = .3;

  // How far each floor above the ground oversails the one below - a jetty -
  // and how far the ground floor is set back behind posts on the street, an
  // arcade. Both come from the standing; the arcade is drawn for, so a street
  // has a few and not a row of them.
  double jettyMetres = 0;
  double arcadeDepthMetres = 0;
  double arcadeChance = 0;

  // Which free ends of the roof rise to a wall rather than fall off it.
  //
  // A standing is a way of building, and the roof is the most visible part of
  // it: a slum is a terrace of gable ends between party walls, a grand house
  // is hipped all round because that is what polite building looked like, and
  // most of what is in between is a hipped range with gabled cross-wings. A
  // join gables whatever this says - see RoofStyle.
  RoofStyle roofStyle = RoofStyle::CrossGabled;
};

// Fills a brief's numbers from its standing, leaving its geometry alone.
void describeStanding(PlotBrief &);

// A piece of ground, with whatever it surrounds cut out of it.
//
// A ring is not enough. The house stands inset from its lot on every side, so
// the ground it leaves is the lot with the house as a hole in it - and a ring
// of that ground is the whole lot. Every yard in the town came out the size
// of its lot that way, and the sum of house and yard exceeded the ground by
// exactly one house.
struct PlotGround
{
  std::vector<rewrite::Position> outer;
  std::vector<std::vector<rewrite::Position>> holes;
};

// One built thing on a plot: its wings, the plates it stacks into, the roof
// over it and whatever carries an overhang.
struct PlotBuilding
{
  std::vector<rewrite::DwellingWing> wings;
  std::vector<rewrite::DwellingRoofFace> roofs;
  std::vector<rewrite::DwellingStorey> levels;
  std::vector<rewrite::DwellingPillar> pillars;
};

struct PlotPlan
{
  // The house: its outline, as the union of its wings, and the built form
  // itself. Empty when the lot could not hold one.
  std::vector<rewrite::Position> house;

  // The courts the house wraps around.
  //
  // A steading's yard is a cell of the plan, not what is left over, so the
  // house outline is a ring and the court is a hole in it. Dropped, the court
  // counts as built: the ground accounting says the plot is covered and the
  // render puts a roof over the yard.
  std::vector<std::vector<rewrite::Position>> courts;
  PlotBuilding built;
  uint32_t storeys = 0;

  // Where the front door is: on the house outline, nearest the street.
  rewrite::Position entrance{};

  // The rest of the ground, each as polygons.
  std::vector<std::vector<rewrite::Position>> outbuildings;
  std::vector<PlotBuilding> sheds; // One per outbuilding, in the same order.
  std::vector<PlotGround> garden; // Ornamental ground behind the house.
  std::vector<PlotGround> yard;   // Working ground: everything open that is not garden.
  std::vector<PlotGround> fence;  // Thin bands on the boundary of a fenced plot.
  std::vector<PlotGround> path;   // From the street to the door.

  double lotArea = 0, houseArea = 0, openArea = 0;

  // Why there is no house, when there is none.
  std::string note;
};

// Plans a lot. Returns false only on a geometric failure; a lot that is simply
// too small for a house returns true with `note` set and no house.
bool planPlot(const PlotBrief &, const ComplexDwellingSettings &, PlotPlan &, std::string *error = nullptr);

} // namespace procedural::world::generator
