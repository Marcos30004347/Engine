#pragma once

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
    double minimumAreaMetres = 200;

    // How far a cut may wander from the middle and from perpendicular.
    double gridChaos = .55;

    // Spread of lot sizes, as the exponent of a log-uniform draw.
    double sizeChaos = .6;

    // Chance a finished lot stays empty - a yard rather than a building.
    double emptyProbability = .04;

    // Watabou's ALLEY, 0.6 units.
    double alleyMetres = 1.5;

    uint32_t maximumDepth = 12;
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
