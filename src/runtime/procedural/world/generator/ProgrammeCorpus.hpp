#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "procedural/world/generator/ArchitecturalProgram.hpp"

namespace procedural::world::generator {

// The transcribed corpus: architectural programmes read off the plates and
// specifications of *The Builder's Practical Director*.
//
// Stage A of .plan/bayesian-architectural-program.md. The paper manually
// encoded 120 programmes from a catalogue of residential layouts and fitted
// its networks to them; this is the same thing done to a 19th-century English
// builder's book, which is the corpus we have and the one that matches the
// town being generated.
//
// The files are checked in under assets/procedural/programmes, one JSON per
// design, because they are the training data and regenerating them means
// reading 784 page scans again by eye.
//
// Two things about them are worth knowing before fitting anything:
//
//   **A room with no dimension has no dimension.** Plenty of rooms are named
//   on a plate and not measured - a water closet, a coal cellar, a closet
//   under the stairs. `dimensioned` says which, and a fit that treats an
//   absent dimension as a zero will put every closet in the smallest bin.
//
//   **`floorsDrawn` says which floors the source showed.** A basement plan
//   with no ground plan is not a house with no ground floor. Absence of a
//   floor from a record is absence of evidence, and counting it as evidence
//   of absence is how you learn that half of all houses have no bedrooms.
struct CorpusRoom
{
  // The normalised vocabulary, as written in the file: "kitchen", "scullery",
  // "dining_room". Normalisation happens once, at transcription, and the raw
  // label is kept beside it.
  std::string use;

  // What distinguishes this instance from others of the same use on the same
  // plate: "beer" and "wine" for two cellars, "front" and "back" for two
  // parlours. Part of the key an adjacency refers to.
  std::string qualifier;

  // The label as it is printed, spelling and all.
  std::string raw;

  int32_t floor = 0;

  bool dimensioned = false;
  double feetLong = 0, feetShort = 0;  // Long side first, whatever the plate's order.

  double areaMetres = 0;
  double aspect = 1;

  // The slot in the generator's vocabulary, or RoomUse::Count when there is
  // none. The corpus names rooms the generator has no enum for - a butler's
  // room, a brewhouse, a servants' hall - and which of those earn a slot is
  // decided by how many plates carry them, not in advance.
  RoomUse mapped = RoomUse::Count;

  // "use" or "use:qualifier"; what an adjacency names.
  std::string key() const { return qualifier.empty() ? use : use + ":" + qualifier; }
};

// Something on the outside of the building that is not a room: a porch, a bay
// window, a tower, a veranda, dormers in the roof.
struct CorpusFeature
{
  std::string kind;
  uint32_t count = 1;
  bool dimensioned = false;
  double feetLong = 0, feetShort = 0;
};

// The building as a solid.
//
// A programme is not only a list of rooms. The paper took the footprint as
// given and generated the interior; here the network owns the footprint, so
// the massing has to be learnable too - which means the corpus has to carry
// what the *elevation* shows and not only what the plan does. A pair of
// semi-detached gabled villas with a conservatory on the flank and a bay to
// the street is a different building from a hipped labourer's cottage of the
// same floor area, and nothing in a room list says so.
//
// Everything here is optional. A specification page states a frontage and a
// depth and nothing else; a plate shows a roof and a tower and leaves the
// dimensions to the scale bar. `known()` says whether the footprint is there.
struct CorpusMassing
{
  // detached, semi-detached, terrace, row, flats.
  std::string attachment;

  // rectangle, double-pile, L, T, U, cross, row.
  std::string plan;

  // hipped, gabled, cross-gabled, hipped-with-gables, parapet, mansard.
  std::string roof;

  // Feet. Frontage is the street face, whether or not it is the longer side -
  // which is the point: a cottage on a narrow slip is deep and narrow, and a
  // bounding box with no sense of which way it faces loses that.
  double frontageFeet = 0, depthFeet = 0;

  // The allotment the design is sold for, where the source names one. These
  // houses are advertised by frontage - "a frontage of twenty-five feet,
  // leaving a gateway entrance of nine feet" - and the gap between this and
  // `frontageFeet` is the gateway or the side passage. It is what a plot
  // planner needs to know before it can place one.
  double plotFrontageFeet = 0;

  // Storeys inside the roof: the attic bedrooms a gabled cottage gets for
  // nothing, which the storey count does not see.
  uint32_t storeysInRoof = 0;

  std::vector<CorpusFeature> features;

  std::string confidence;

  bool known() const { return frontageFeet > 0 && depthFeet > 0; }
  double areaSqFt() const { return frontageFeet * depthFeet; }
  bool has(const std::string &kind) const;
};

struct CorpusLink
{
  uint32_t a = 0, b = 0;
  bool door = true;
};

struct CorpusProgramme
{
  uint32_t page = 0;
  std::string id;      // "plate-161a"
  std::string kind;    // "labourer's cottage", "farm house and offices"
  std::string source;
  std::string notes;

  uint32_t storeys = 1;
  bool basement = false;

  // Whether this building is a house. A few records are not - a warehouse, a
  // factory - and are here because a town has them in it. Anything reasoning
  // about dwellings has to leave them out, and the alternative to a flag is
  // matching on the `kind` string, which is prose.
  bool dwelling = true;

  // Which levels the source actually drew or described. -1 is a basement.
  std::vector<int32_t> floorsDrawn;

  // An inference flagged at transcription: another page that looks like
  // another floor of the same house. Zero when there is none.
  uint32_t sameHouseAs = 0;

  std::string roomConfidence, dimensionConfidence, adjacencyConfidence;

  // How the adjacencies were got, which matters more than how sure we are,
  // because it decides whether a *missing* door is evidence:
  //
  //   "stated"    the source names the doors in prose - "the front door opens
  //               into a small passage ... on the left is the entrance to the
  //               living room". Absence is evidence.
  //   "openings"  read off the plate as breaks in the internal wall poché. A
  //               solid wall with no break is a wall, so absence is evidence.
  //   "inferred"  taken from which rooms adjoin and how the plan works.
  //               Absence is not evidence and must not be counted.
  //
  // What none of them give is the door swing, which these plates do not draw
  // at their scale - so whether a link is a door or an open wall comes from
  // the prose where the prose says, and is a door by default where it does not.
  std::string adjacencySource;

  CorpusMassing massing;

  std::vector<CorpusRoom> rooms;
  std::vector<CorpusLink> links;

  uint32_t countOf(const std::string &use) const;
  bool drewFloor(int32_t level) const;
};

// Loads every plate-*.json in `directory`. False, with `why` set, if any file
// is malformed or names an adjacency between rooms that are not there - the
// transcription is hand-written and a typo in it is a silent corruption of
// the training data, so it is an error and not a warning.
bool loadProgrammeCorpus(const std::string &directory,
                         std::vector<CorpusProgramme> &out,
                         std::string &why);

// The generator's slot for a corpus vocabulary word, or RoomUse::Count.
RoomUse roomUseFromCorpusName(const std::string &);

}  // namespace procedural::world::generator
