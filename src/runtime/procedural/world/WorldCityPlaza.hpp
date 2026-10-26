#pragma once

#include "WorldGenerationContracts.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::rewrite {

// A public square, as what stands on it.
//
// A square used to be a polygon of paving with a row of stalls down one side
// and a hall across the other, and it read as a car park: flat to its edges,
// open to every street at every point, nothing to say where it began. The
// squares this is drawn from are not like that. Hondarribia's is closed by
// arcades and entered under an arch; Dubrovnik's has a loggia down one side
// and a gate through the wall; the market places of León and a hundred other
// towns are stepped up to a cross or a fountain in the middle, so that the
// square has a centre and the centre has a height.
//
// So a square is four things, each of which is geometry the city model can
// build: a stepped ground (terraces climbing to a dais, with flights of broad
// steps between them), an enclosure (an arcade of piers and arches round the
// edge, with a parapet above), gates (a taller arch where each street comes
// in, with a storey over it), and a monument at the centre (a fountain, a
// market cross or a well). The stalls and the hall stay, inside the arcade.
//
// Everything is in the local frame of the settlement, in metres, with y the
// height above `level`. The city model adds `level` when it meshes it.

struct CityPlazaEntry
{
  Position at;           // On the square's boundary.
  Position inward;       // Unit vector into the square.
  double widthMetres = 5; // Clear width of the gate.
};

struct CityPlazaTerrace
{
  std::vector<Position> ring; // Counter-clockwise.
  double level = 0;           // Top of the paving above the square's ground.
};

// A flight of broad, shallow steps up one edge of a terrace: from the `foot`
// edge at the lower level to the `head` edge at the upper one. The treads
// are cut between the two edges.
struct CityPlazaFlight
{
  Position footLeft, footRight, headLeft, headRight;
  double lower = 0, upper = 0;
  uint32_t risers = 1;
};

// One bay of the enclosure: a pier at `a`, an arch spanning to the pier at
// `b`, the attic above. The pier at `b` belongs to the next bay (or to the
// corner). `inward` is the square's side of the bay.
struct CityPlazaBay
{
  Position a, b;
  Position inward;
  double pierMetres = .6;       // Pier side, square in plan.
  double springingMetres = 3.;  // Height of the arch's springing line.
  double apexMetres = 4.4;      // Height of the arch's crown.
  double topMetres = 5.2;       // Top of the attic.
  double parapetMetres = 0;     // A low wall under the arch; nought for an open bay.
  bool gate = false;            // A street enters here: taller, wider, with a storey over.
  uint32_t entry = UINT32_MAX;  // Which entry, for a gate.
};

// A house on the square, behind the arcade: the arcade is its covered walk
// and its front stands on the walk's back line. A row shares party walls;
// `role` is a block lot role name the city's completion already reads
// (shop-house, inn, pub, bakehouse, workshop).
struct CityPlazaLot
{
  std::vector<Position> ring; // Counter-clockwise.
  std::string role = "shop-house";
  bool attached = true;
  uint32_t side = 0; // Which enclosure edge it fronts.
};

enum class CityPlazaMonument : uint8_t
{
  None,
  Fountain, // A basin, a bowl and a jet.
  Cross,    // A shaft on steps with a cross at the top.
  Well      // A low ring with a frame and a little roof.
};

struct CityPlazaPavilion
{
  Position centre;
  Position along{1, 0, 0};
  double width = 5.4, depth = 3.6;
};

struct CityPlazaPlan
{
  uint64_t id = 0;
  std::vector<Position> ring; // The square, counter-clockwise.
  double level = 0;           // Ground height of the square in the settlement frame.
  Position centre;

  std::vector<CityPlazaEntry> entries;
  std::vector<CityPlazaTerrace> terraces; // Lowest first; the square's own ground is not listed.
  std::vector<CityPlazaFlight> flights;
  std::vector<CityPlazaBay> bays;
  std::vector<Position> corners; // Where the enclosure turns: a square pier at each.

  CityPlazaMonument monument = CityPlazaMonument::None;
  double monumentRadius = 0, monumentHeight = 0;

  std::vector<std::vector<Position>> stalls; // Shambles bays, in a glued run.
  std::vector<std::vector<Position>> halls;  // The market hall.
  std::vector<Position> gardens; // Planted seating islands; 2.4 m reserved radius.
  std::vector<CityPlazaPavilion> pavilions; // Covered public market tables, clear of gate approaches.
  std::vector<CityPlazaLot> lots;            // Houses behind the arcade, on the sides that have room.
  std::vector<std::string> notes;
};

} // namespace procedural::world::rewrite
