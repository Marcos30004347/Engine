#pragma once

#include "Generator.hpp"
#include "TownStreets.hpp"
#include "WardBuildings.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// Blocks after Watabou's Ward.getCityBlock: every patch, cut where an artery
// runs through its interior, inset edge by edge (half a main street on the
// wall, an artery or the plaza; half a regular street inside the walls, half
// an alley outside) is a block. A patch holding a compound (the temple's, the
// hall's) is that compound's yard and gets no block. For the outskirts
// filter every block carries its populated edges (roads 1, edges beside a
// walled patch 1, beside another outskirts patch .4) and vertex densities
// (gate 1, vertex touching only city patches 2 * random, else 0).
struct TownBlocksInput
{
  std::vector<TownPatch> patches; // After the street moves.
  std::vector<std::vector<Position>> arteries;
  // Authored curve samples reserve the full road ribbon before lot allocation.
  std::vector<std::vector<Position>> curvedArteries;
  ConvexRing wallRing; // Empty when unwalled.
  std::vector<Position> gates;
  std::vector<ConvexRing> compounds;
  double arteryHalfWidth = 2.5;
  double mainStreet = 5, regularStreet = 2.5, alley = 1.5;
  // Dense urban patches are not themselves building blocks. Split them with
  // local streets before lots are allocated; zero disables this refinement.
  double maximumInteriorBlockArea = 1800, maximumInteriorBlockSpan = 60;

  // A CELL EDGE IS NOT A STREET.
  //
  // With this on, an edge of the planning lattice becomes a street only where
  // the routed network actually runs along it - the wall, an artery, the
  // plaza's frontage, or a lane the service pass routed to reach a block that
  // no street had reached. Everything else dissolves: patches that share an
  // unclaimed edge are one block, and the block is bounded by roads rather
  // than by the Voronoi diagram it happens to be tiled from.
  //
  // Off, every patch edge is a lane and every block is a cell, which is what
  // made the whole town read as its own partition.
  bool dissolveCells = true;

  // How far the service pass may route to reach an unserved block.
  double serviceReachMetres = 260;

  // The market square: how wide it is, and how much of the plaza cell it may
  // take. A town's centre is a square, not a hexagonal planning cell left
  // blank and not a cell built over like any other - the square IS the
  // market, and the blocks stand round it.
  double marketSideMetres = 46;
  double marketShare = .45;
  uint64_t seed = 1;
  std::function<double(uint64_t, uint64_t)> random;
};

struct TownBlock
{
  uint64_t patch = 0;
  ConvexRing ring;

  // Whether the ring may be concave, which a dissolved block usually is: it
  // is the land between streets, not a cell. The lot stage picks a
  // subdivision that can cut one.
  bool dissolved = false;
  bool withinWalls = false, outskirts = false;
  uint8_t use = 7;
  std::vector<WardEdgePopulation> populated; // Absolute edges of the patch piece.
  std::vector<double> vertexDensity;         // Per ring vertex.
};

struct TownBlocksPlan
{
  struct Lane { Position a, b; double halfWidth = 1; bool arterial = false; };
  std::vector<Lane> lanes;

  // The market square, when the town has a plaza cell: open ground, paved,
  // with the blocks cut around it.
  ConvexRing market;
  std::vector<TownBlock> blocks;
  uint32_t yards = 0, cuts = 0;
};

class TownBlocks final : public Generator<TownBlocksInput, TownBlocksPlan>
{
public:
  explicit TownBlocks(uint64_t domain);

  static bool plan(const TownBlocksInput &input, TownBlocksPlan &output, Ledger &ledger, std::string *error);

protected:
  bool run(const TownBlocksInput &input, TownBlocksPlan &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
