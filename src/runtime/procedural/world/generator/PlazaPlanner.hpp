#pragma once

#include "Generator.hpp"
#include "procedural/world/WorldCityPlaza.hpp"
#include "procedural/world/hierarchy/Generator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::generator {

using rewrite::CityPlazaMonument;
using rewrite::CityPlazaPlan;
using rewrite::Position;

// What a square is asked to be.
//
// The ring is the paved reservation the town set aside, counter-clockwise in
// the settlement's local frame. The entries are where streets reach it; left
// empty, the planner puts one in the middle of every side long enough to
// carry a gate, because a square nobody can get into is a courtyard.
struct PlazaBrief
{
  uint64_t id = 1, seed = 7;
  std::vector<Position> ring;
  std::vector<rewrite::CityPlazaEntry> entries;
  double level = 0;

  // The enclosure: an arcade standing just inside the boundary, bay by bay,
  // closed under every arch by a parapet except at the gates.
  bool enclosure = true;
  double insetMetres = .9;
  double bayMetres = 3.4;
  double pierMetres = .6;
  double springingMetres = 3.;
  double topMetres = 5.2;
  double parapetMetres = 2.2;  // The wall under each arch of a closed bay; the arch above it is an open gallery.
  double gateClearMetres = 5.;
  double gateSpringingMetres = 3.8;
  double gateTopMetres = 9.4;  // The gatehouse: a storey over the arch, between two turrets.

  // The ground: a dais of broad concentric steps at the centre, with the
  // monument on it. The dais is a share of the inradius, capped, and never
  // reaches the stalls.
  bool terraces = true;
  uint32_t daisSteps = 3;
  double stepRiseMetres = .16;
  double stepTreadMetres = .42;
  double daisShare = .38;
  double daisMaximumMetres = 11.;
  double daisMinimumMetres = 4.;

  // Houses behind the arcade. A side carries a row when the square has the
  // depth for it: the walk under the arcade, the lot, and a square still
  // worth the name inside. Every side when the inradius allows, the two
  // longest sides when only they do, none on a small square. The houses are
  // the square's trade, so a housed square has no shambles and no hall.
  bool houses = true;
  double arcadeWalkMetres = 2.4;
  double lotDepthMetres = 8.;
  double frontageMetres = 5.;
  double housedInradiusMetres = 17.;
  double twoSidedInradiusMetres = 12.5;

  // The civic hall: the biggest front on the square, in the middle of the
  // side across from the inn's, beside the gate if one is in its way.
  bool civicHall = true;
  double civicHallWidthMetres = 14.;
  double civicHallDepthMetres = 11.;

  // The monument. `Auto` picks one from the seed.
  bool monumentAuto = true;
  CityPlazaMonument monument = CityPlazaMonument::Fountain;

  // What trades on the square: a glued run of shambles bays down the longest
  // side and a market hall across from it, both inside the arcade.
  double shamblesFrontageMetres = 3.8;
  double shamblesDepthMetres = 7.5;
  double marketHallWidthMetres = 15.;
  double marketHallDepthMetres = 9.5;
  bool shambles = true;
  bool marketHall = true;
};

class PlazaPlanner final : public Generator<PlazaBrief, CityPlazaPlan>
{
public:
  explicit PlazaPlanner(uint64_t domain);

  // Pure: the same brief gives the same square.
  static bool plan(const PlazaBrief &, CityPlazaPlan &, Ledger &, std::string *error = nullptr);

protected:
  bool run(const PlazaBrief &, CityPlazaPlan &, Ledger &, std::string *error) override;
};

// The square's structures as faces: terraces, arcade, gates, monument. In
// the plan's local frame with y above the square's ground; the stalls and
// the hall are lots and are built by the city like any other building.
void meshPlaza(const CityPlazaPlan &, hierarchy::Solid &);

} // namespace procedural::world::generator
