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
  ConvexRing wallRing; // Empty when unwalled.
  std::vector<Position> gates;
  std::vector<ConvexRing> compounds;
  double arteryHalfWidth = 2.5;
  double mainStreet = 5, regularStreet = 2.5, alley = 1.5;
  uint64_t seed = 1;
  std::function<double(uint64_t, uint64_t)> random;
};

struct TownBlock
{
  uint64_t patch = 0;
  ConvexRing ring;
  bool withinWalls = false, outskirts = false;
  uint8_t use = 7;
  std::vector<WardEdgePopulation> populated; // Absolute edges of the patch piece.
  std::vector<double> vertexDensity;         // Per ring vertex.
};

struct TownBlocksPlan
{
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
