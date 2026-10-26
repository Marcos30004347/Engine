#pragma once

#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/WorldDwellingPlan.hpp"
#include "procedural/world/generator/CellStreet.hpp"
#include "procedural/world/generator/Ledger.hpp"

#include <functional>
#include <string>
#include <vector>

namespace procedural::world::generator {

// A neighbourhood: buildings that share their circulation.
//
// Two rows of stacks facing each other across an alley, every storey of every
// stack a household of its own, the upper ones reached from outside - a
// gallery along the alley face on each level, and a dog-leg stair in the gap
// between two stacks that serves both of them. Read `docs/neighbourhood-plan.md`
// before changing any rule here: each one below cost a debugging session in
// the planner this replaces, and every one of them looked right in a render.
//
// The model is the plan's (P20): a DECK is any level surface you can stand on
// - the alley, a gallery, a landing - and a FLIGHT joins two decks. One name
// for every surface is what keeps "can you walk from here to there" a single
// graph search, and the invariant that matters is that every front door in the
// neighbourhood is in ONE component of that graph, seeded from one root and
// proved by the walk rather than assumed from adjacency.
//
enum class DeckKind : uint8_t
{
  Alley,   // The open ground between the rows, and the gaps between stacks.
  Gallery, // Along the alley face of a row at one level; facing galleries meet.
  Landing  // The half landing of a dog-leg stair.
};

const char *name(DeckKind);

struct NeighbourhoodDeck
{
  DeckKind kind = DeckKind::Alley;
  std::vector<rewrite::Position> footprint; // Counter-clockwise, metres, at `level`.
  double level = 0;
  uint32_t row = 0;    // Galleries and gaps belong to a row; the alley to neither (0).
  uint32_t storey = 0; // Galleries: which storey they serve.

  // Height of the (possibly graded) surface at a world point. `level` remains
  // its representative height for sorting and diagnostics; the footprint's
  // vertex heights are authoritative on sloped ground.
  double levelAt(double x, double z) const;
};

struct NeighbourhoodPlinth
{
  uint32_t row = 0, plot = 0;
  std::vector<rewrite::Position> footprint;
  double base = 0, top = 0;
};

// A household: one storey of one stack, and where its door is.
struct CellUnit
{
  uint32_t row = 0;
  uint32_t plot = 0;
  uint32_t storey = 0;
  uint32_t deck = 0;         // The deck its door opens onto.
  rewrite::Position door{};  // The door's middle, at the deck's level.
};

// A dog-leg stair in one gap: two flights and a half landing per storey,
// climbing from the ground to the top gallery the gap's neighbours reach.
struct NeighbourhoodStair
{
  uint32_t row = 0;
  uint32_t afterPlot = 0; // The gap after this plot (UINT32_MAX: before the first).
  double u0 = 0, u1 = 0;  // The gap, along the alley.
  std::vector<rewrite::DwellingStair> flights;
  std::vector<std::pair<uint32_t, uint32_t>> joins; // Deck pairs each storey's pair of flights joins.
};

struct CellNeighbourhoodBrief
{
  uint64_t id = 0;
  uint64_t seed = 1;

  // The alley's axis, in metres, left to right. Row 0 stands to the left of
  // it and row 1 to the right, the alley between.
  geom2::Point a{};
  geom2::Point b{};
  double alleyMetres = 2.6;

  // How deep each row's ground is behind its alley face.
  double depthMetres = 9;

  BuildingClass building = BuildingClass::Dwelling;
  uint32_t storeys = 3;
  uint32_t gapCells = 1;

  // World-space ground height. Empty means level zero. Buildings stand
  // rigidly on the sample at the middle of their frontage while circulation
  // surfaces carry the sampled grade in their vertices.
  std::function<double(double, double)> terrain;
};

struct CellNeighbourhood
{
  uint64_t id = 0;
  geom2::Point a{};
  geom2::Point b{};
  double alleyMetres = 0;
  double cellMetres = 0;
  CellStreet rows[2];
  std::vector<NeighbourhoodDeck> decks;
  std::vector<NeighbourhoodStair> stairs;
  std::vector<CellUnit> units;
  std::vector<NeighbourhoodPlinth> plinths;

  // What the walk found: how many components the deck graph has, and how many
  // units' doors are in the one the alley is in. `solved` is both being right.
  uint32_t islands = 0;
  uint32_t reachedUnits = 0;
  bool solved = false;
};

bool planCellNeighbourhood(const CellNeighbourhoodBrief &, const CellDwellingSettings &, CellNeighbourhood &, Ledger &, std::string *error = nullptr);

// Every stack realised, with its upper doors cut, its galleries and rails, and
// the stairs of the gaps it borders. One plan per plot, row 0's plots first.
bool realiseCellNeighbourhood(const CellNeighbourhood &, const CellDwellingSettings &, std::vector<rewrite::DwellingPlan> &, std::string *error = nullptr);

} // namespace procedural::world::generator
