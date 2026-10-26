#pragma once

#include "procedural/world/citylayout/LayoutGeometry.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace procedural::world::citylayout {

using rewrite::CityLayout;
using rewrite::CitySettings;

// How a city's blocks are filled with buildings.
//
// generateCityLayout is 4,593 lines, and about half of it is one
// `if (s.blockHousing)` branch: two complete, independent ways of turning the
// street network into houses, chosen by a boolean, with a two-thousand-line
// preamble shared between them.
//
// That boolean is the "different lot allocation generators" this refactor set
// out to replace. As classes the two strategies are peers with a name each,
// the choice is a binding rather than a flag, and a third can be added without
// touching either.

// What the shared preamble produced, and what a strategy is allowed to touch.
//
// Bound by reference at the branch point rather than copied: the preamble
// builds these on its own stack and the strategy is the last thing to run, so
// naming them here is what turns "everything in scope" into a stated contract.
struct LayoutBuild
{
  const CitySettings &settings;

  // The layout under construction. A strategy adds objects, frontages and
  // contours to it; the streets, wards and walls are already final.
  CityLayout &layout;

  // Ward cells after every cut, merge and re-cut the preamble applied.
  const std::vector<Poly> &cells;

  // Road ribbons already published as ground, so a strategy never places a
  // building on one.
  const std::vector<Poly> &roads;

  // Masks the wall and its towers occupy.
  const std::vector<Poly> &wallMasks;

  // Land inside the layout's envelope that a building may stand on, as convex
  // pieces. A strategy tests against these rather than re-deriving them.
  const std::vector<Poly> &usable;

  // Ward ids at or beyond this index sit outside the wall, where the rules
  // for frontage and density differ.
  uint64_t firstOuterWard = 0;

  // Deterministic unit draw, addressed by feature and purpose rather than by
  // call order.
  const std::function<double(uint64_t, uint64_t)> &random;

  // Height of the terrace the streets and houses were levelled onto - not the
  // untreated ground, which is a different surface.
  const std::function<double(double, double)> &terrain;

  // Ground already spoken for: compounds, landmark precincts, anything the
  // caller reserved before the layout ran.
  const std::vector<Poly> &reserved;

  // The wall line as a ring, empty when the settlement is unwalled.
  const Poly &wallRing;

  // Where roads leave the settlement. Outer wards gather their buildings
  // along the roads that reach these.
  const std::vector<rewrite::CityGate> &gates;

  // Bounding box of the layout envelope, which several passes bin against.
  double minX = 0;
  double minZ = 0;
  double maxX = 0;
  double maxZ = 0;
};

// One way of filling a city's blocks.
//
// A strategy runs last and owns the result: it returns false only on a hard
// failure such as an exceeded budget, and records ordinary shortfalls - a
// block too small, a gap it could not fill - on the layout itself.
class HousingStrategy
{
public:
  virtual ~HousingStrategy() = default;

  // Stable identity, used to bind the strategy and to explain a result.
  virtual const char *name() const = 0;

  virtual bool allocate(LayoutBuild &, std::string *error) = 0;
};

using StrategyPtr = std::shared_ptr<HousingStrategy>;

// Partitions each street block into lots and fits a shell in each.
//
// The richer of the two: it extracts the faces of the road arrangement as
// blocks, keeps their interiors as shared courts, cuts lots against the
// frontage, and fits a regular shell inside each lot. This is what a town or
// a city uses, where the ground is worth subdividing carefully.
class BlockHousingStrategy final : public HousingStrategy
{
public:
  const char *name() const override
  {
    return "housing.blocks";
  }

  bool allocate(LayoutBuild &, std::string *error) override;
};

// Walks each street by arc length and places buildings along it.
//
// The simpler of the two, and the older: it advances a cursor along every
// retained street path, places the widest shell that fits the remaining
// frontage, and records an explicit gap where none does. What a village uses,
// where buildings follow the road rather than the road enclosing blocks.
class FrontageHousingStrategy final : public HousingStrategy
{
public:
  const char *name() const override
  {
    return "housing.frontage";
  }

  bool allocate(LayoutBuild &, std::string *error) override;
};

} // namespace procedural::world::citylayout
