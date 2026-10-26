#pragma once

#include "procedural/world/generator/PlotPlanner.hpp"
#include "procedural/world/generator/ProgramDwellingPlanner.hpp"
#include "procedural/world/hierarchy/Composer.hpp"

namespace procedural::world::hierarchy {

// A first set of generators against the three interfaces.
//
// Two bind to Role::Streets and two to Role::Lots, which is the point: the
// choice between them is a binding in the policy, decided per region, and not
// a boolean with a precedence rule. Nothing here is new algorithm work - these
// are the shapes the settlement generator already makes - but they are the
// first written *as* the interface rather than as a free function called from
// an orchestrator.

// Streets on a grid: a road every `spacingMetres` along two perpendicular
// axes, each a widthed edge, so the roads come back as regions of their own.
class GridStreets final : public EdgeGenerator
{
public:
  struct Settings
  {
    double spacingMetres = 40;
    double halfWidthMetres = 3;
    double axisRadians = 0;
  };

  GridStreets() = default;

  explicit GridStreets(const Settings &settings) : settings_(settings)
  {
  }

  Descriptor descriptor() const override;

  bool propose(const Request &, std::vector<Edge> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// Streets radiating from the region's centre, with a ring road partway out.
// The same role, a different shape, and the caller picks by name.
class RadialStreets final : public EdgeGenerator
{
public:
  struct Settings
  {
    uint32_t spokes = 6;
    double ringFraction = .55;
    double halfWidthMetres = 3;
  };

  RadialStreets() = default;

  explicit RadialStreets(const Settings &settings) : settings_(settings)
  {
  }

  Descriptor descriptor() const override;

  bool propose(const Request &, std::vector<Edge> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// Lots with a frontage and a depth, cut on both axes.
//
// Zero-width cuts: a lot boundary divides without consuming ground, unlike a
// street. The depth comes from the target area at the frontage ratio and the
// width from the area at that depth, so a lot stays a sensible shape however
// large the block is - cutting only the long axis turns a big block into
// slivers, which is what the first version did.
class StripLots final : public EdgeGenerator
{
public:
  struct Settings
  {
    double targetAreaMetres = 200;
    double jitter = .25;

    // How much wider than deep a lot is. Frontage is the scarce thing on a
    // street, so a lot is usually deeper than it is wide; above 1 this is the
    // other way round.
    double frontageRatio = 1.6;
  };

  StripLots() = default;

  explicit StripLots(const Settings &settings) : settings_(settings)
  {
  }

  Descriptor descriptor() const override;

  bool propose(const Request &, std::vector<Edge> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// City blocks as a relaxed Voronoi patch layer, after Watabou's town generator.
//
// The town generator has no imposed road grid. It lays patches - a relaxed
// Voronoi diagram over the settlement - and the streets *are the shared edges
// of those patches*. A block is a patch inset from its own edges, and a ward is
// a block with a lot rule. That ordering is why a Watabou town looks like a
// town and an imposed grid does not: the streets inherit the irregularity of
// the patch layer instead of the patch layer inheriting the regularity of a
// grid.
//
// Lloyd relaxation is what keeps the cells from being either a lattice or a
// mess. None at all and a blue-noise site set still gives lopsided cells with
// long thin necks; too much and every cell converges on a hexagon and the town
// is a honeycomb. A couple of passes is the usable middle, which is why the
// count is a setting rather than a constant.
//
// Pair this with BisectionLots on the blocks it leaves: patches for the street
// pattern, createAlleys for what happens inside each one.
class VoronoiBlocks final : public EdgeGenerator
{
public:
  struct Settings
  {
    // Distance between patch sites: the block size, near enough.
    double spacingMetres = 55;

    // Half width of an ordinary street: the shared edge between two blocks.
    double halfWidthMetres = 3.5;

    // Half width of an artery.
    //
    // Watabou's town does not draw every patch edge at one width. Its inset
    // table has MAIN_STREET at twice REGULAR_STREET and four times ALLEY, and
    // the main streets are not an arbitrary subset: they are the routes from
    // the gates to the middle of the town, traced through the patch graph.
    // Drawing every shared edge identically is what makes a render read as "a
    // Voronoi diagram" rather than as a town, because a town's street pattern
    // is a hierarchy and a Voronoi diagram is not.
    double arteryHalfWidthMetres = 6.0;

    // How many gates the arteries run from. Each traces the cheapest route
    // through the patch graph from a block on the edge of the town to the
    // block nearest its middle, and every shared edge along that route is
    // widened.
    uint32_t arteries = 4;

    // Lloyd passes over the central patches only. Zero leaves the spiral as
    // laid; Watabou uses three.
    uint32_t relaxIterations = 3;

    // Junctions closer than this are merged into one corner, after
    // Model.optimizeJunctions. Zero derives it from the spacing.
    double mergeJunctionsMetres = 0;

    // A patch with more sides than this gets a street through its middle,
    // square to its longest edge. A many-sided Voronoi cell is a big one, and a
    // big one holds too many houses behind one frontage. Zero disables.
    uint32_t splitSidesAbove = 5;

    // Half width of that street. Zero derives it: between a street and an
    // artery.
    double splitHalfWidthMetres = 0;

    // Passes of smoothStreet over each artery. Each pass moves every interior
    // corner of the route toward the mean of its neighbours, bending the
    // patches with it.
    uint32_t smoothingPasses = 1;

    // The weight on the corner itself in that mean: (v0 + f * v1 + v2) / (2 + f).
    //
    // Watabou uses 3, which pulls a corner only two fifths of the way toward
    // its neighbours - enough for his A* routes, which are long. Lower is a
    // stronger pull and a straighter main street. It cannot go to zero: the
    // corners are shared with the patches, so smoothing hard enough moves a
    // patch's own vertices past each other and the block turns inside out.
    double smoothingWeight = 3.0;
  };

  VoronoiBlocks() = default;

  explicit VoronoiBlocks(const Settings &settings) : settings_(settings)
  {
  }

  Descriptor descriptor() const override;

  bool propose(const Request &, std::vector<Edge> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// Lots by recursive bisection, after Watabou's Ward.createAlleys.
//
// Ported from the TownGeneratorOS source rather than inferred from its output.
// Five rules, each load-bearing:
//
//  1. Cut the block's *longest edge*, perpendicular to it, so every lot takes
//     its frontage on the short side - which is what a lot is.
//  2. The cut lands at `ratio`, centred on the middle and spread by
//     0.8 * gridChaos, so at zero chaos every cut halves exactly.
//  3. The cut is skewed by up to pi/6 * gridChaos - but *only while the piece
//     is still bigger than four times the target*. The last two levels are
//     perpendicular, so the final lots are rectangles. This is the rule that
//     keeps an organic town from dissolving into triangles.
//  4. A piece stops splitting below `minimumArea * 2^(4 * sizeChaos * (r-.5))`
//     - log-uniform, so a block holds a *mix* of sizes rather than one size
//     with jitter on it.
//  5. The alley gap only opens while the piece is large relative to a random
//     draw. Big groups get alleys between them; neighbours within a group
//     touch. That is what makes a terrace.
//
// Emits each bisection chord as an edge - zero width for a lot line, banded
// when an alley opens - so the composer's arrangement reproduces exactly the
// subdivision the recursion described.
class BisectionLots final : public EdgeGenerator
{
public:
  struct Settings
  {
    // Watabou's minSq, in square metres. His unit is 2.5 m, so his minSq of 10
    // to 110 units squared is 62 to 690 m2.
    //
    // He does not use one number: every ward draws its own, as
    // `base + spread * r * r`, and r * r biases the draw toward the base while
    // letting a few wards be generous. Pinning it at the base - which is what
    // the presets did - made every craftsmen block the smallest a craftsmen
    // block can be, and the render showed it: lots averaging 30 m2 where his
    // average about 230.
    double minimumAreaMetres = 200;
    double minimumAreaSpreadMetres = 0;

    // How far a cut may stray from halving the area, as a fraction.
    //
    // Watabou cuts at a ratio along the longest edge, which halves the area
    // only when the piece is a rectangle - on a trapezoid it does not, and on a
    // wedge it is nowhere close. Halving the area and varying *that* makes a
    // lot a deliberate share of its parent rather than an accident of its
    // shape, and is what stops a tapered block from producing one fat lot and
    // one thin one at every level.
    double areaVariation = .18;

    // A cut is only taken if both halves fill this much of their own bounding
    // rectangle. A rectangle fills 1, a triangle 0.5.
    double minimumFillFraction = .58;

    // ...and if neither half is narrower than this. Fill alone accepts a
    // sliver, which fills its rectangle perfectly and is still a sliver.
    double minimumWidthMetres = 3.5;

    // How square a piece must be before its area is allowed to finish it.
    //
    // Halving is a sequence - a square cut once is two halves of 1:2, cut
    // again four squares - and stopping on area alone stops wherever that
    // sequence happens to be. A piece longer than this keeps splitting across
    // its length however small it is, until it is square enough or too small
    // to halve.
    double squareEnoughRatio = .62;

    // How much a cut is judged on the squareness of the lots it makes, rather
    // than only on how well they fill their rectangles.
    //
    // Halving the area fixes wedges and leaves terraces: a long block cut
    // repeatedly across its length reaches the target area as a row of 1:6
    // strips, every one of which fills its rectangle perfectly and is still
    // the wrong shape for a plot. Watabou avoids it implicitly by always
    // cutting the longest edge; scoring it makes the same preference explicit
    // and lets it be traded against fill. Zero ignores shape, one makes a
    // square the only acceptable lot.
    double squarenessWeight = .55;

    // No cut may leave a piece smaller than this, whatever the stop rule says.
    //
    // The stop rule is log-uniform about minSq and the alley ribbon takes a
    // strip off every half, so a cut near the stop size routinely left a piece
    // of five or ten square metres that nothing could stand on. Measured, 199
    // of 202 unbuilt lots in a town were under 25 m2, and the cut that made
    // each of them should not have been made. This is that: a piece too small
    // to build on is not cut off, it stays part of its neighbour.
    double minimumLotMetres = 30;

    // How far a cut may wander from the middle and from perpendicular.
    double gridChaos = .55;

    // Spread of lot sizes, as the exponent of a log-uniform draw.
    double sizeChaos = .6;

    // Chance a finished lot stays empty - a yard rather than a building.
    double emptyProbability = .04;

    // Watabou's ALLEY, 0.6 units.
    double alleyMetres = 1.5;

    uint32_t maximumDepth = 12;

    // Keep every cut on one of two fixed axes, after Watabou's
    // createOrthoBuilding.
    //
    // createAlleys cuts perpendicular to the *current* piece's longest edge.
    // That edge changes at every level, so the cut direction drifts, and a
    // piece that has been cut four times can have four unrelated directions in
    // it - which is where a wedge comes from. createOrthoBuilding instead fixes
    // two perpendicular axes once, from the block's own longest edge, and at
    // each step cuts along whichever of the two is more perpendicular to the
    // piece's longest edge. Every cut is then parallel or square to every other
    // cut, so the pieces stay rectangles. Watabou's own note on it is "no fans,
    // ever".
    //
    // The cost is regularity: a block subdivided this way reads as planned
    // ground rather than as something that grew. That is right for a lot and
    // wrong for a ward boundary, which is why it is a mode and not the rule.
    bool orthogonal = false;

    // Width of the first cut's ribbon, when it is a street rather than an
    // alley. A rich ward's block is served by a proper street through its
    // middle; a craftsmen or slum block is served by alleys. Zero means the
    // alley rule applies at every depth.
    double streetMetres = 0;

    // The ward this parameter set is, used as the binding id - "lots.bisection"
    // alone when empty, "lots.bisection.craftsmen" otherwise. Watabou's town
    // has no single lot rule: createWards hands each patch a ward class, and
    // the class is four numbers. Several sets have to be bound at once for a
    // policy to choose between them, and a registry binds by id.
    std::string ward;
  };

  BisectionLots() = default;

  explicit BisectionLots(const Settings &settings) : settings_(settings)
  {
  }

  // The six ward characters from the town generator, as named presets. The
  // difference between a slum and a patrician quarter is four numbers.
  static Settings craftsmen();

  static Settings merchant();

  static Settings patriciate();

  static Settings administration();

  static Settings slum();

  Descriptor descriptor() const override;

  bool propose(const Request &, std::vector<Edge> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// Lots rung around a common court: the block is copied inward by the lot depth
// and the band between is cut radially. The court in the middle is a region
// like any other, so something can be put in it.
class RingLots final : public EdgeGenerator
{
public:
  struct Settings
  {
    double depthMetres = 12;
    double targetAreaMetres = 120;

    // Which ward this is, used as the binding id - "lots.ring" alone when
    // empty, "lots.ring.<ward>" otherwise. A patrician court and a temple
    // cloister are the same rule at different sizes, and a registry binds by
    // id.
    std::string ward;
  };

  RingLots() = default;

  explicit RingLots(const Settings &settings) : settings_(settings)
  {
  }

  Descriptor descriptor() const override;

  bool propose(const Request &, std::vector<Edge> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// A house on each lot, inset from its boundary.
class HouseFootprints final : public RegionPopulator
{
public:
  struct Settings
  {
    double setbackMetres = 1.5;
    double minimumAreaMetres = 20;
    double storeyMetres = 3;

    // Largest share of its lot a house may cover. The setback alone is not
    // enough: a metre and a half off a large lot still leaves a building the
    // size of the lot, which no settlement looks like. Zero disables the cap.
    double maximumCoverage = .45;

    // A piece larger than this is common ground, not a building plot.
    //
    // A courtyard block leaves its middle as one open piece, and the peel that
    // produced it has no way to say so - it is a face like any other. Built on,
    // it becomes a single building covering half the block, which is what the
    // town render showed: a courtyard block with a warehouse in its court.
    // Bounding the plot size is what tells the difference, because a plot and a
    // court differ by an order of magnitude in area and by nothing else this
    // generator can see. Zero disables the bound.
    double maximumAreaMetres = 0;

    // The plot's standing, which the lot planner turns into how much of the
    // ground the house takes, how many storeys it has, whether there is a
    // garden and what stands in it. See PlotPlanner.hpp; the fields below
    // override its numbers where they are set.
    procedural::world::generator::PlotStanding standing = procedural::world::generator::PlotStanding::Modest;

    // Zero leaves the standing's own value.
    uint32_t minimumStoreys = 0, maximumStoreys = 0;

    // What a lot does with the ground the house does not cover.
    //
    // A poor plot builds over all of it - that is what poor means, and a slum
    // where every house has a garden is not a slum. A rich plot does the
    // opposite: the house takes a share of the frontage, the rest is garden,
    // and the service buildings stand at the back of it. Watabou's mansion is
    // the same idea at a larger scale, wings and courts rather than a house
    // and a yard, and the thing that distinguishes them is how much of the
    // plot is *not* built on.
    enum class Yard : uint8_t
    {
      None,    // Built to the boundary.
      Garden,  // Open ground behind the house.
      Grounds, // Garden, and outbuildings standing in it.

      // A yard for animals, fenced, with a barn in it.
      //
      // What a plot outside the walls does with its ground. A house in a
      // village or on the outskirts is not a smaller town house: most of its
      // plot is working ground - a pen, a byre, a midden - and the fence is
      // the thing that makes it read as one from above. Watabou's own inset
      // table says the same in miniature: a patch outside the walls is inset
      // by an ALLEY where one inside is inset by a REGULAR_STREET, because
      // what happens between buildings out there is different.
      Paddock
    };

    Yard yard = Yard::None;

    // Whether the house gets an interior, and how it is planned.
    //
    // `PlotPlanner` composes the exterior and says the interior is the next
    // layer's problem; the programme generator is that layer, and this is
    // where a town asks for it. What it buys is a house that has rooms - and
    // therefore a front door in a room, walls where the rooms are, and a
    // household with a size and a trade - rather than a shell with a roof.
    //
    // The two programme settings differ only in how the rooms are placed, and
    // the difference is two orders of magnitude: the power diagram costs about
    // three milliseconds a house, which a town of two hundred can afford; the
    // paper's §5 optimisation costs about two thirds of a second, which it
    // cannot. Spend that only on a building someone will stand inside.
    enum class Interior : uint8_t
    {
      None,               // A shell. What the town built before this existed.
      Programme,          // Rooms by weighted power diagram: ~3 ms.
      ProgrammeOptimised, // Rooms by Metropolis, after Merrell §5: ~700 ms.
    };

    Interior interior = Interior::None;

    // What stands on the lot, when the interior is planned. A ward knows this
    // - a craftsmen's ward is shop houses and a merchant's is commerce houses -
    // and the network has one per class.
    procedural::world::generator::BuildingClass building =
      procedural::world::generator::BuildingClass::Dwelling;

    // The rail of a paddock fence, and how high it stands.
    double fenceThicknessMetres = .25;
    double fenceHeightMetres = 1.2;

    // The ward this parameter set is, used as the binding id -
    // "buildings.setback" alone when empty.
    std::string ward;
  };

  HouseFootprints() = default;

  explicit HouseFootprints(const Settings &settings) : settings_(settings)
  {
  }

  Descriptor descriptor() const override;

  bool populate(const Request &, std::vector<Object> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// Houses along a lane, after Watabou's Village Generator.
//
// A village is not a town at a smaller size. The author states it uses the
// Parish-Muller road approach - roads first, buildings hung off them
// afterwards - with no patches and no wards, and two rules follow that the
// town algorithm does not have:
//
//  1. A house sits *perpendicular to its road*, set back from it, rather than
//     taking whatever orientation the block it happens to fall in has. A
//     village house faces the lane it is on; a town house faces the block it
//     is in.
//  2. A secondary driveway connects each house back to the primary road, so
//     the frontage is an access, not just an aspect.
//
// Subdividing the faces of a road graph, which is what the town path does,
// cannot express either: it produces lots whose orientation comes from the
// face, and access is implied by adjacency rather than drawn. So this walks the
// region's road frontage by arc length instead, and places each house against
// it.
class RoadsideHouses final : public RegionPopulator
{
public:
  struct Settings
  {
    // Along the road, and away from it. Frontage is the scarce thing on a
    // lane, so a village house is wider than it is deep - it turns its long
    // side to the road, which is what makes a street read as a street.
    double frontageMetres = 7;
    double depthMetres = 5.5;

    // Gap between neighbours, measured along the road.
    double spacingMetres = 6;

    // How far the front wall stands back from the road edge. The driveway
    // fills this.
    double setbackMetres = 3.5;

    double drivewayWidthMetres = 2.0;

    // How far a house may be nudged along its road and how much its setback may
    // vary, as fractions of the spacing and the setback. A lane of identically
    // placed houses reads as a terrace, not as a village.
    double jitter = .35;

    double storeyMetres = 3;
  };

  RoadsideHouses() = default;

  explicit RoadsideHouses(const Settings &settings) : settings_(settings)
  {
  }

  Descriptor descriptor() const override;

  bool populate(const Request &, std::vector<Object> &out, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

// Which ward each block is, after Model.createWards.
//
// A list of ward classes is consumed in order - Watabou's WARDS, a fifteen
// patch town being eleven craftsmen, one merchant, one temple, one
// administration and one slum - and each class rates every unassigned block
// and takes the best: merchant nearest the plaza, administration bordering it,
// the temple beside it weighted to the largest, slums farthest away, patriciate
// away from slums. The plaza is the block holding the town's middle, and is
// never built on.
//
// A function rather than a set of policy rules with distance bands and id
// modulos in them, because that is what it is in the source, and because the
// bands were where the bugs lived.
class WardAssignment
{
public:
  // The blocks are the children of `root` that exist when this is first asked,
  // which is after the street step has run. Returns "plaza" for the plaza.
  const std::string &wardOf(const RegionTree &, RegionId root, const Region &block, const geom2::Point &centre, uint64_t seed);

private:
  void assign(const RegionTree &, RegionId root, const geom2::Point &centre, uint64_t seed);

  bool assigned_ = false;
  std::map<RegionId, std::string> byRegion_;
};

// Reads the terrain callback and writes slope and buildability onto regions.
// Geometry untouched - which is the whole distinction this interface exists to
// make.
class SlopeField final : public FieldGenerator
{
public:
  struct Settings
  {
    double unbuildableSlope = .25;
  };

  SlopeField() = default;

  explicit SlopeField(const Settings &settings) : settings_(settings)
  {
  }

  Descriptor descriptor() const override;

  bool evaluate(const RegionTree &, const Context &, std::vector<Region *> &, Ledger &, std::string *error) override;

private:
  Settings settings_{};
};

} // namespace procedural::world::hierarchy
