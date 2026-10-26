#include "procedural/world/generator/ArchitecturalProgram.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>
#include <iterator>

namespace procedural::world::generator {

using learn::BayesianNetwork;
using learn::Evidence;

namespace {

constexpr uint32_t roomUseCount = uint32_t(RoomUse::Count);
constexpr uint32_t standingCount = 6;
constexpr double sqFtToSqM = 0.09290304;

// Reference house: a comfortable two-storey one. Every room's typical area is
// quoted for this house and scaled from it, so changing what "comfortable"
// means moves the whole table together instead of one room at a time.
constexpr double referenceTotalSqFt = 1250.;

// --- bins ----------------------------------------------------------------

// Room area, per the plan: 25 sq ft steps to 200, then 50 to 600, then a tail.
// The tail's upper edge is nominal - it exists so a draw inside the last bin
// has somewhere to land - and everything above it is that bin.
const std::vector<double> kAreaEdges = {
  8, 25, 50, 75, 100, 125, 150, 175, 200,
  250, 300, 350, 400, 450, 500, 550, 600, 1200};

// Whole-house square footage. Coarser than the room bins because there are
// only ever a few hundred plates to fit it from.
const std::vector<double> kTotalEdges = {150, 300, 450, 600, 900, 1300, 1900, 2800, 4200, 7000};

// 1:1, 5:4, 3:2, 2:1, and everything longer.
const std::vector<double> kAspects = {1.0, 1.25, 1.5, 2.0, 2.6};

uint32_t binOf(const std::vector<double> &edges, const double value)
{
  const uint32_t bins = uint32_t(edges.size()) - 1;

  for (uint32_t i = 0; i < bins; ++i)
  {
    if (value < edges[i + 1])
    {
      return i;
    }
  }

  return bins - 1;
}

// A bin's representative value: the geometric middle, because these are all
// quantities whose spread is multiplicative.
double representative(const std::vector<double> &edges, const uint32_t bin)
{
  const double lo = std::max(edges[bin], edges[1] * .25);

  return std::sqrt(lo * edges[bin + 1]);
}

double normalCdf(const double x)
{
  return .5 * std::erfc(-x * 0.70710678118654752440);
}

// A lognormal over a bin ladder: the weight of each bin is the mass the
// distribution puts between its edges. This is the plan's "quantise before
// fitting", done to a prior instead of to counts, and it is the one place a
// hand-authored network can state a smooth belief rather than a table.
std::vector<double> lognormalOverBins(const std::vector<double> &edges,
                                      const double mean,
                                      const double sigma)
{
  const uint32_t bins = uint32_t(edges.size()) - 1;
  const double mu = std::log(std::max(mean, 1e-3));

  std::vector<double> weights(bins, 0.);
  double sum = 0.;

  for (uint32_t i = 0; i < bins; ++i)
  {
    const double lo = i == 0 ? 0. : normalCdf((std::log(edges[i]) - mu) / sigma);
    const double hi = i + 1 == bins ? 1. : normalCdf((std::log(edges[i + 1]) - mu) / sigma);

    weights[i] = std::max(hi - lo, 0.);
    sum += weights[i];
  }

  // A floor, so no bin is impossible. A zero in a CPT is a hard constraint,
  // and "no kitchen was ever this big in 120 plates" is not one.
  for (double &w : weights)
  {
    w = w / std::max(sum, 1e-12) + 1e-4;
  }

  return weights;
}

std::vector<std::string> numberedStates(const uint32_t first, const uint32_t last)
{
  std::vector<std::string> states;

  for (uint32_t i = first; i <= last; ++i)
  {
    states.push_back(std::to_string(i));
  }

  return states;
}

std::vector<std::string> binStates(const std::vector<double> &edges)
{
  std::vector<std::string> states;

  for (size_t i = 0; i + 1 < edges.size(); ++i)
  {
    states.push_back(std::to_string(int(edges[i])) + "-" + std::to_string(int(edges[i + 1])));
  }

  return states;
}

// --- the room table ------------------------------------------------------

// Where a room sits in the section. A house of this period is stratified and
// the stratification is not a preference: the service rooms are below or
// behind, the polite rooms are on the ground at the front, the bedrooms are
// up. This is the privacy gradient the paper's §3 is about, stated once.
enum class Band : uint8_t
{
  Service,  // Basement when there is one, ground otherwise.
  Ground,
  Upper,    // Ground when the house has only one storey.
  Cellar,   // Basement, or nothing.
};

struct RoomPrior
{
  RoomUse use = RoomUse::Kitchen;

  // Typical area in the reference house, square feet, and the spread of the
  // lognormal about it.
  double typicalSqFt = 100;
  double sigma = .35;

  // How the room grows with the house. A kitchen in a mansion is bigger than
  // one in a cottage but not four times bigger; a drawing room is. A closet
  // barely moves at all.
  double growth = .5;

  // 0 puts the aspect ratio at 1:1, 1 pushes it to the long tail. A closet
  // and a passage are long and thin whatever else is true of them.
  double elongation = .25;

  Band band = Band::Ground;

  // Probability the room exists, by standing:
  // Slum, Modest, Comfortable, Wealthy, Grand, Rural.
  std::array<double, standingCount> exists = {0, 0, 0, 0, 0, 0};
};

// The numbers are priors, not counts. They come from how these houses work
// and from the corpus's own room frequencies over 182 candidate plates
// (kitchen 80, hall 70, closet 59, bedroom 57, wc 57, cellar 52, scullery 40,
// staircase 39, passage 37, parlour 35, chamber 29, pantry 23, landing 23,
// larder 22, dining room 22, dressing room 21). Stage B1 replaces them with
// maximum-likelihood estimates from the transcriptions; the node names do not
// change, so the replacement is a table swap.
const std::array<RoomPrior, roomUseCount> &roomPriors()
{
  static const std::array<RoomPrior, roomUseCount> table = [] {
    std::array<RoomPrior, roomUseCount> t{};

    auto put = [&t](const RoomPrior &p) { t[size_t(p.use)] = p; };

    // The countryside, and housing people in numbers.
    //
    // A byre is sized for the beasts it holds - two cows and their standing is
    // twenty square metres - and a dormitory for the bodies, at four square
    // metres each, which is what a straw pallet and the room to stand beside
    // it actually took.
    // All of these are off in the shared table and switched on by the classes
    // that have them. A room in the vocabulary with no partner in the pair
    // table is stranded every time it is drawn, and the repair rate is what
    // catches it - so a byre belongs to the cottage that has a byre, not to
    // every house in the book.
    put({RoomUse::Byre,       210, .28, .30, .35, Band::Service, {.00, .00, .00, .00, .00, .00}});
    put({RoomUse::Croft,      900, .25, .60, .40, Band::Service, {.00, .00, .00, .00, .00, .00}});
    put({RoomUse::Garden,    1600, .22, .70, .35, Band::Service, {.00, .00, .00, .00, .00, .00}});
    put({RoomUse::Orchard,   2400, .20, .80, .30, Band::Service, {.00, .00, .00, .00, .00, .00}});
    put({RoomUse::Pen,        320, .26, .45, .40, Band::Service, {.00, .00, .00, .00, .00, .00}});
    put({RoomUse::Midden,      40, .30, .25, .50, Band::Service, {.00, .00, .00, .00, .00, .00}});
    put({RoomUse::Dormitory,  420, .26, .40, .25, Band::Upper, {.00, .00, .00, .00, .00, .00}});
    put({RoomUse::Refectory,  300, .28, .35, .25, Band::Ground, {.00, .00, .00, .00, .00, .00}});

    // Service.
    put({RoomUse::Kitchen,    190, .32, .35, .20, Band::Service, {1., 1., 1., 1., 1., 1.}});
    put({RoomUse::Scullery,   110, .30, .25, .30, Band::Service, {.05, .35, .75, .95, .98, .55}});
    put({RoomUse::Larder,      55, .30, .20, .45, Band::Service, {.02, .20, .55, .85, .95, .45}});
    put({RoomUse::Pantry,      45, .32, .20, .45, Band::Service, {.00, .10, .35, .70, .85, .15}});
    put({RoomUse::Dairy,       80, .30, .20, .35, Band::Service, {.00, .02, .05, .15, .25, .55}});
    put({RoomUse::Washhouse,  100, .32, .25, .30, Band::Service, {.00, .10, .30, .55, .65, .50}});
    put({RoomUse::Cellar,      90, .40, .30, .35, Band::Cellar,  {.05, .25, .60, .85, .95, .30}});

    // Polite. These are the rooms that grow with the house.
    put({RoomUse::Parlour,      170, .30, .55, .20, Band::Ground, {.15, .60, .85, .75, .45, .55}});
    // A living room belongs to a house with no servants: it is where the
    // family sits, eats and works, and a house grand enough to have a parlour
    // *and* a dining room has stopped needing one.
    put({RoomUse::LivingRoom,   190, .28, .40, .22, Band::Ground, {.45, .55, .30, .08, .02, .70}});
    put({RoomUse::DiningRoom,   210, .30, .60, .22, Band::Ground, {.00, .05, .45, .90, .98, .15}});
    put({RoomUse::DrawingRoom,  250, .32, .70, .25, Band::Ground, {.00, .00, .15, .75, .95, .05}});
    put({RoomUse::BreakfastRoom,150, .30, .50, .22, Band::Ground, {.00, .00, .05, .35, .70, .02}});
    put({RoomUse::Study,        130, .30, .45, .22, Band::Ground, {.00, .02, .10, .45, .75, .05}});
    put({RoomUse::Library,      190, .28, .55, .20, Band::Ground, {.00, .00, .05, .40, .80, .02}});

    // Circulation.
    put({RoomUse::Hall,       120, .40, .55, .55, Band::Ground, {.10, .45, .85, .98, .99, .40}});
    put({RoomUse::Passage,     70, .45, .45, .90, Band::Ground, {.05, .20, .45, .80, .92, .25}});
    put({RoomUse::Landing,     70, .40, .45, .70, Band::Upper,  {.02, .35, .70, .90, .95, .25}});
    put({RoomUse::Staircase,   55, .30, .25, .75, Band::Ground, {.25, .55, .85, .95, .98, .45}});
    put({RoomUse::Lobby,       45, .35, .30, .40, Band::Ground, {.00, .05, .15, .40, .60, .05}});
    put({RoomUse::Porch,       35, .30, .25, .35, Band::Ground, {.02, .15, .30, .55, .70, .35}});

    // Private.
    put({RoomUse::Bedroom,     150, .35, .45, .25, Band::Upper, {.90, .98, 1., 1., 1., .98}});
    put({RoomUse::DressingRoom, 70, .30, .30, .35, Band::Upper, {.00, .05, .20, .60, .85, .05}});
    put({RoomUse::Nursery,     120, .30, .35, .25, Band::Upper, {.00, .02, .12, .45, .70, .05}});
    put({RoomUse::Closet,       30, .35, .15, .70, Band::Upper, {.20, .55, .80, .92, .95, .50}});
    put({RoomUse::WaterCloset,  25, .25, .10, .50, Band::Ground, {.10, .35, .70, .95, .98, .40}});
    put({RoomUse::Bathroom,     55, .28, .20, .35, Band::Upper, {.00, .02, .08, .35, .65, .03}});

    // A large house's rooms. They exist only at the top of the scale, which
    // is what the standing column says.
    put({RoomUse::MorningRoom,  180, .28, .55, .22, Band::Ground, {.00, .00, .02, .30, .65, .02}});
    put({RoomUse::Boudoir,      130, .28, .45, .25, Band::Upper,  {.00, .00, .00, .15, .45, .00}});
    put({RoomUse::Conservatory, 150, .35, .50, .45, Band::Ground, {.00, .00, .05, .30, .55, .05}});
    put({RoomUse::Veranda,      140, .40, .40, .70, Band::Ground, {.00, .02, .08, .30, .45, .15}});
    put({RoomUse::ServantsHall, 200, .28, .35, .25, Band::Service,{.00, .00, .00, .25, .70, .05}});
    put({RoomUse::Brewhouse,    120, .30, .25, .30, Band::Service,{.00, .00, .02, .10, .20, .40}});

    // A public house.
    put({RoomUse::Bar,           80, .35, .30, .45, Band::Ground, {.90, .95, .98, .98, .98, .90}});
    put({RoomUse::TapRoom,      150, .30, .30, .30, Band::Ground, {.85, .85, .75, .60, .45, .85}});
    put({RoomUse::ClubRoom,     700, .35, .45, .55, Band::Upper,  {.10, .30, .60, .85, .95, .20}});
    put({RoomUse::CoffeeRoom,   150, .28, .30, .28, Band::Upper,  {.15, .35, .65, .85, .95, .25}});
    put({RoomUse::BilliardRoom, 170, .25, .25, .35, Band::Upper,  {.02, .10, .35, .70, .90, .05}});

    // A school and a dispensary.
    put({RoomUse::SchoolRoom,   900, .30, .40, .45, Band::Ground, {.98, .98, .98, .98, .98, .98}});
    put({RoomUse::Dispensary,   180, .28, .30, .30, Band::Ground, {.95, .95, .95, .95, .95, .95}});
    put({RoomUse::WaitingRoom,  220, .30, .35, .35, Band::Ground, {.90, .92, .95, .95, .95, .90}});
    put({RoomUse::ConsultingRoom,140,.25, .25, .28, Band::Ground, {.85, .90, .95, .95, .95, .85}});

    // A farmstead's working buildings.
    put({RoomUse::Stable,       350, .35, .30, .45, Band::Ground, {.00, .05, .15, .45, .70, .85}});
    put({RoomUse::Barn,         800, .40, .35, .45, Band::Ground, {.00, .00, .05, .20, .40, .80}});
    put({RoomUse::Loft,         300, .40, .35, .50, Band::Upper,  {.00, .02, .08, .20, .30, .60}});

    // A church. The nave is the building; everything else is measured against
    // it, which is why its typical area is an order above a room's.
    put({RoomUse::Nave,        1400, .40, .55, .45, Band::Ground, {.98, .98, .98, .98, .98, .98}});
    put({RoomUse::Chancel,      420, .35, .50, .35, Band::Ground, {.70, .75, .85, .92, .95, .80}});
    put({RoomUse::Aisle,        500, .40, .55, .80, Band::Ground, {.25, .35, .55, .75, .88, .35}});
    put({RoomUse::Transept,     380, .35, .50, .35, Band::Ground, {.05, .10, .25, .50, .75, .10}});
    put({RoomUse::Sanctuary,    180, .30, .40, .30, Band::Ground, {.55, .60, .70, .80, .88, .60}});
    put({RoomUse::Narthex,      160, .35, .40, .55, Band::Ground, {.30, .38, .50, .65, .78, .35}});
    put({RoomUse::SideChapel,   220, .30, .40, .35, Band::Ground, {.02, .05, .15, .40, .70, .05}});
    put({RoomUse::Vestry,       130, .30, .30, .30, Band::Ground, {.70, .78, .85, .90, .92, .75}});
    put({RoomUse::Crypt,        500, .40, .45, .40, Band::Cellar, {.05, .10, .25, .50, .72, .10}});
    put({RoomUse::Belfry,       110, .25, .25, .25, Band::Upper,  {.35, .45, .62, .78, .88, .45}});
    put({RoomUse::Cloister,     700, .40, .50, .90, Band::Ground, {.00, .02, .08, .28, .55, .05}});
    put({RoomUse::ChapterHouse, 400, .30, .40, .28, Band::Ground, {.00, .00, .04, .18, .45, .02}});
    put({RoomUse::Gallery,      450, .40, .50, .95, Band::Upper,  {.30, .40, .55, .70, .80, .35}});

    // Castle and medieval hall.
    put({RoomUse::GreatHall,    900, .35, .50, .35, Band::Ground, {.60, .70, .85, .95, .98, .75}});
    put({RoomUse::ScreensPassage,110,.30, .30, .95, Band::Ground, {.55, .62, .75, .85, .92, .65}});
    put({RoomUse::Solar,        320, .30, .45, .30, Band::Upper,  {.50, .60, .75, .88, .95, .62}});
    put({RoomUse::Buttery,      130, .28, .30, .40, Band::Service,{.55, .62, .75, .85, .92, .65}});
    put({RoomUse::Keep,         800, .35, .45, .25, Band::Ground, {.70, .75, .82, .90, .95, .70}});
    put({RoomUse::Gatehouse,    350, .30, .35, .35, Band::Ground, {.60, .70, .80, .90, .95, .55}});
    put({RoomUse::Guardroom,    200, .30, .30, .32, Band::Ground, {.65, .72, .82, .90, .95, .55}});
    put({RoomUse::Barracks,     600, .35, .40, .45, Band::Upper,  {.30, .42, .58, .75, .88, .30}});
    put({RoomUse::Armoury,      250, .30, .35, .35, Band::Ground, {.25, .35, .52, .72, .88, .25}});
    put({RoomUse::Dungeon,      180, .35, .30, .35, Band::Cellar, {.30, .38, .50, .65, .80, .25}});
    put({RoomUse::Tower,        260, .30, .35, .20, Band::Ground, {.55, .65, .78, .88, .95, .50}});
    put({RoomUse::Courtyard,   1600, .45, .50, .35, Band::Ground, {.45, .58, .72, .85, .95, .65}});
    put({RoomUse::Well,          30, .30, .15, .20, Band::Ground, {.55, .62, .72, .82, .90, .75}});

    // A prison.
    put({RoomUse::Cell,          70, .18, .12, .45, Band::Ground, {.98, .98, .98, .98, .98, .98}});
    put({RoomUse::Infirmary,    320, .30, .35, .35, Band::Ground, {.35, .48, .65, .80, .90, .35}});
    put({RoomUse::ExerciseYard,1200, .40, .45, .55, Band::Ground, {.70, .78, .88, .92, .95, .75}});

    // A mansion's state rooms.
    put({RoomUse::Saloon,       520, .30, .55, .28, Band::Ground, {.00, .00, .02, .35, .85, .02}});
    put({RoomUse::Ballroom,     900, .30, .55, .40, Band::Ground, {.00, .00, .00, .12, .60, .00}});
    put({RoomUse::SmokingRoom,  200, .28, .40, .30, Band::Ground, {.00, .00, .05, .35, .70, .02}});
    put({RoomUse::GunRoom,      140, .28, .30, .32, Band::Ground, {.00, .00, .02, .20, .50, .12}});

    // ...and its service establishment.
    put({RoomUse::StillRoom,    150, .28, .30, .32, Band::Service,{.00, .00, .02, .28, .72, .05}});
    put({RoomUse::HousekeepersRoom,180,.28,.30, .30, Band::Service,{.00, .00, .02, .32, .80, .05}});
    put({RoomUse::ButlersRoom,  170, .28, .30, .30, Band::Service,{.00, .00, .04, .38, .82, .05}});

    // Trade.
    put({RoomUse::CountingHouse, 150, .28, .35, .30, Band::Ground, {.35, .50, .70, .85, .90, .30}});
    put({RoomUse::StrongRoom,     50, .25, .20, .25, Band::Ground, {.20, .32, .52, .72, .85, .15}});
    put({RoomUse::CouncilChamber,420, .28, .40, .30, Band::Upper,  {.55, .65, .80, .90, .95, .50}});
    put({RoomUse::Undercroft,    700, .35, .45, .40, Band::Cellar, {.45, .55, .70, .82, .90, .40}});

    // Working.
    put({RoomUse::Office,      90, .35, .35, .30, Band::Ground, {.00, .05, .10, .25, .45, .10}});
    put({RoomUse::Shop,       220, .40, .30, .35, Band::Ground, {.20, .15, .10, .02, .00, .02}});
    put({RoomUse::Workshop,   180, .40, .30, .35, Band::Ground, {.10, .20, .15, .05, .02, .20}});
    put({RoomUse::Store,       50, .40, .25, .45, Band::Ground, {.05, .15, .30, .50, .65, .45}});
    put({RoomUse::Yard,       300, .55, .40, .45, Band::Ground, {.30, .55, .60, .55, .45, .85}});

    return t;
  }();

  return table;
}

// --- the adjacency pairs -------------------------------------------------

struct PairPrior
{
  RoomUse a, b;
  double adjacent;  // p(direct access | both rooms exist)
  double door;      // p(it is a door rather than an open wall | adjacent)

  // Rooms whose presence all but closes this door.
  //
  // A chamber opens off the living room in a cottage because there is nowhere
  // else for it to open off. Give the same house a hall and a landing and
  // that door is gone - the bedroom goes upstairs and is reached from the
  // landing - so the pair is not simply unlikely, it is *conditionally*
  // unlikely, and a network is the thing that can say so. Authored as extra
  // parents on the adjacency node rather than as a lower probability, because
  // a lower probability would put a kitchen door on a mansion's bedroom one
  // house in five.
  std::vector<RoomUse> closedBy = {};
  double whenClosed = .02;
};

// Which rooms can open into which, and how likely it is that they do.
//
// A pair that is absent from this list can never be joined, and that is how
// the privacy gradient is stated here rather than hoped for: there is no
// bedroom-kitchen pair and no kitchen-bathroom pair, so no sampled programme
// can ask for the kitchen-bathroom-bedroom path the paper names as the thing
// independent per-pair probabilities produce.
//
// Circulation carries the graph. The hall and the passage are what most of
// the ground floor reaches, the landing is what every bedroom reaches, and
// the service rooms chain off the kitchen and the scullery.
const std::vector<PairPrior> &pairPriors()
{
  static const std::vector<PairPrior> pairs = {
    // The hall is the hub of the ground floor.
    {RoomUse::Hall, RoomUse::Porch,        .85, .90},
    {RoomUse::Hall, RoomUse::Staircase,    .90, .10},
    {RoomUse::Hall, RoomUse::Parlour,      .92, .95},
    {RoomUse::Hall, RoomUse::DiningRoom,   .85, .95},
    {RoomUse::Hall, RoomUse::DrawingRoom,  .88, .95},
    {RoomUse::Hall, RoomUse::Study,        .80, .95},
    {RoomUse::Hall, RoomUse::Library,      .82, .95},
    {RoomUse::Hall, RoomUse::LivingRoom,   .85, .95},
    {RoomUse::Hall, RoomUse::BreakfastRoom, .55, .95},
    {RoomUse::Hall, RoomUse::Store,        .35, .95},
    {RoomUse::Hall, RoomUse::Kitchen,      .70, .95},
    {RoomUse::Hall, RoomUse::Bedroom,      .90, 1.0},
    // A cottage on one floor puts its chambers off the living rooms, because
    // there is nowhere else to put them. It is the plan a labourer's cottage
    // actually has, and without it every such house comes out with bedrooms
    // no door reaches.
    {RoomUse::Parlour, RoomUse::Bedroom,   .72, 1.0, {RoomUse::Hall, RoomUse::Landing}, .02},
    {RoomUse::Kitchen, RoomUse::Bedroom,   .58, 1.0, {RoomUse::Hall, RoomUse::Landing}, .01},
    {RoomUse::Hall, RoomUse::Passage,      .85, .20},
    {RoomUse::Hall, RoomUse::Lobby,        .75, .50},
    {RoomUse::Hall, RoomUse::WaterCloset,  .40, 1.0},
    {RoomUse::Hall, RoomUse::Office,       .70, .95},
    {RoomUse::Hall, RoomUse::Shop,         .70, .95},

    // The passage is the hall's reach into the back of the house.
    {RoomUse::Passage, RoomUse::Kitchen,     .80, .90},
    {RoomUse::Passage, RoomUse::Parlour,     .55, .95},
    {RoomUse::Passage, RoomUse::Bedroom,     .60, 1.0},
    {RoomUse::Passage, RoomUse::Staircase,   .70, .20},
    {RoomUse::Passage, RoomUse::WaterCloset, .70, 1.0},
    {RoomUse::Passage, RoomUse::Store,       .55, .95},

    // The service chain. kitchen+scullery is the corpus's strongest
    // co-occurrence after the circulation pairs (29 of 182 plates).
    {RoomUse::Kitchen, RoomUse::Scullery,      .92, .85},
    {RoomUse::Kitchen, RoomUse::Larder,        .80, 1.0},
    {RoomUse::Kitchen, RoomUse::Pantry,        .72, 1.0},
    {RoomUse::Kitchen, RoomUse::Cellar,        .55, 1.0},
    {RoomUse::Kitchen, RoomUse::Washhouse,     .55, .95},
    {RoomUse::Kitchen, RoomUse::Yard,          .80, .95},
    {RoomUse::Kitchen, RoomUse::DiningRoom,    .30, 1.0},
    {RoomUse::Kitchen, RoomUse::BreakfastRoom, .55, 1.0},
    {RoomUse::Kitchen, RoomUse::Parlour,       .70, 1.0},
    {RoomUse::Kitchen, RoomUse::Porch,         .50, .90},
    {RoomUse::Kitchen, RoomUse::Store,         .45, .95},
    {RoomUse::Kitchen, RoomUse::Workshop,      .50, .90},
    {RoomUse::Kitchen, RoomUse::Shop,          .45, .90},
    {RoomUse::Kitchen, RoomUse::Lobby,         .30, .90},
    {RoomUse::Kitchen, RoomUse::Dairy,         .45, 1.0},
    {RoomUse::Scullery, RoomUse::Washhouse,    .70, .90},
    {RoomUse::Scullery, RoomUse::Dairy,        .70, 1.0},
    {RoomUse::Scullery, RoomUse::Larder,       .50, 1.0},
    {RoomUse::Scullery, RoomUse::Cellar,       .50, 1.0},
    {RoomUse::Scullery, RoomUse::Yard,         .75, .95},
    {RoomUse::Scullery, RoomUse::WaterCloset,  .40, 1.0},
    {RoomUse::Scullery, RoomUse::Pantry,       .35, 1.0},
    {RoomUse::Larder, RoomUse::Pantry,         .30, 1.0},
    {RoomUse::Passage, RoomUse::Pantry,        .35, 1.0},
    {RoomUse::Passage, RoomUse::Larder,        .30, 1.0},
    {RoomUse::Passage, RoomUse::Cellar,        .35, .95},
    {RoomUse::Passage, RoomUse::Scullery,      .45, .95},
    {RoomUse::Passage, RoomUse::DiningRoom,    .35, .95},
    {RoomUse::Passage, RoomUse::BreakfastRoom, .40, .95},
    {RoomUse::Passage, RoomUse::Study,         .35, .95},
    {RoomUse::Passage, RoomUse::Closet,        .30, 1.0},

    // The polite rooms open into each other as well as off the hall; the
    // double doors between a dining and a drawing room are the open wall.
    {RoomUse::DiningRoom, RoomUse::DrawingRoom,   .60, .75},
    {RoomUse::DiningRoom, RoomUse::BreakfastRoom, .45, .90},
    {RoomUse::DrawingRoom, RoomUse::Study,        .35, .90},
    {RoomUse::DrawingRoom, RoomUse::Library,      .40, .90},
    {RoomUse::DiningRoom, RoomUse::Library,       .35, .90},
    {RoomUse::Library, RoomUse::Study,            .20, .90},
    {RoomUse::LivingRoom, RoomUse::Kitchen,       .78, .95},
    {RoomUse::LivingRoom, RoomUse::Scullery,      .55, .95},
    {RoomUse::LivingRoom, RoomUse::Parlour,       .50, .95},
    {RoomUse::LivingRoom, RoomUse::Passage,       .70, .95},
    {RoomUse::LivingRoom, RoomUse::Porch,         .40, .90},
    {RoomUse::LivingRoom, RoomUse::Staircase,     .55, .90},
    {RoomUse::LivingRoom, RoomUse::Larder,        .35, 1.0},
    {RoomUse::LivingRoom, RoomUse::Yard,          .35, .95},
    {RoomUse::LivingRoom, RoomUse::Washhouse,     .40, .95},
    // The same cottage rule as the parlour's: a chamber opens off the living
    // room when there is nowhere else, and not otherwise.
    {RoomUse::LivingRoom, RoomUse::Bedroom, .72, 1.0, {RoomUse::Hall, RoomUse::Landing}, .03},
    {RoomUse::LivingRoom, RoomUse::Closet,        .35, 1.0},
    {RoomUse::Parlour, RoomUse::DiningRoom,       .45, .90},

    // Upstairs. Everything private hangs off the landing.
    {RoomUse::Staircase, RoomUse::Bedroom,   .92, 1.0, {RoomUse::Landing}, .30},
    {RoomUse::Staircase, RoomUse::Kitchen,   .75, .90},
    {RoomUse::Staircase, RoomUse::Scullery,  .50, .90},
    {RoomUse::Staircase, RoomUse::Passage,   .70, .30},
    {RoomUse::Staircase, RoomUse::Parlour,   .30, .95},
    {RoomUse::Staircase, RoomUse::Cellar,    .45, .90},
    {RoomUse::Landing, RoomUse::Staircase,   .95, .05},
    {RoomUse::Landing, RoomUse::DressingRoom, .50, 1.0},
    {RoomUse::Landing, RoomUse::Nursery,      .70, 1.0},
    {RoomUse::Landing, RoomUse::Bedroom,     .98, 1.0},
    {RoomUse::Landing, RoomUse::WaterCloset, .45, 1.0},
    {RoomUse::Landing, RoomUse::Bathroom,    .85, 1.0},
    {RoomUse::Landing, RoomUse::Closet,      .68, 1.0},
    {RoomUse::Bedroom, RoomUse::DressingRoom, .80, 1.0},
    {RoomUse::Bedroom, RoomUse::Closet,       .82, 1.0},
    {RoomUse::Bedroom, RoomUse::Nursery,      .55, 1.0},
    {RoomUse::Bedroom, RoomUse::Bathroom,     .25, 1.0},
    {RoomUse::Landing, RoomUse::Store,        .30, .95},
    {RoomUse::DressingRoom, RoomUse::Closet,  .35, 1.0},
    {RoomUse::Nursery, RoomUse::Closet,       .30, 1.0},

    // The yard is a room in these plans and the back door goes to it.
    {RoomUse::Porch, RoomUse::Lobby,         .40, .80},
    {RoomUse::Yard, RoomUse::Washhouse,      .75, .95},
    {RoomUse::Yard, RoomUse::Store,          .65, .95},
    {RoomUse::Yard, RoomUse::Dairy,          .65, .95},
    {RoomUse::Yard, RoomUse::Workshop,       .50, .95},
    {RoomUse::Yard, RoomUse::Porch,          .35, .90},
    {RoomUse::Yard, RoomUse::Larder,         .25, 1.0},
    {RoomUse::Yard, RoomUse::Cellar,         .30, .95},
    {RoomUse::Yard, RoomUse::Pantry,         .20, 1.0},
    {RoomUse::Yard, RoomUse::WaterCloset,    .75, 1.0},
    {RoomUse::Shop, RoomUse::Workshop,       .55, .90},
    {RoomUse::Shop, RoomUse::Porch,          .35, .90},
    {RoomUse::Shop, RoomUse::Parlour,        .30, .95},
    {RoomUse::Workshop, RoomUse::Store,      .40, .95},
    {RoomUse::Workshop, RoomUse::Passage,    .30, .95},
    {RoomUse::WaterCloset, RoomUse::Porch,   .20, 1.0},
    {RoomUse::WaterCloset, RoomUse::Lobby,   .30, 1.0},
    {RoomUse::Lobby, RoomUse::Passage,       .45, .60},
    {RoomUse::Lobby, RoomUse::Staircase,     .40, .30},
    {RoomUse::Store, RoomUse::Cellar,        .30, .95},
    {RoomUse::Office, RoomUse::Study,        .25, .90},
    {RoomUse::Office, RoomUse::Passage,      .40, .95},
    {RoomUse::Shop, RoomUse::Store,          .40, .95},
    {RoomUse::Parlour, RoomUse::Study,       .30, .90},
    {RoomUse::Parlour, RoomUse::Porch,       .25, .90},
    {RoomUse::Bathroom, RoomUse::DressingRoom, .30, 1.0},
    {RoomUse::Washhouse, RoomUse::Store,     .30, .95},
  };

  return pairs;
}

// --- the network ---------------------------------------------------------

// What a class's network is built over.
//
// The room table and the pair table above are the *dwelling's*. A class takes
// a subset of each, and adds the pairs peculiar to it. Everything else - the
// typical areas, the bands, the growth with house size - is shared, because a
// kitchen is a kitchen whichever building it is in.
struct ClassProfile
{
  BuildingClass klass = BuildingClass::Dwelling;

  // The rooms that get nodes. A room absent from here has no existence node,
  // no area node and no aspect node, and no pair naming it survives.
  std::vector<RoomUse> rooms;

  // Pairs beyond the dwelling's, for the rooms peculiar to this class.
  std::vector<PairPrior> extraPairs;

  // The rooms this class must put on the street. See RoomSpec::frontsStreet.
  std::vector<RoomUse> streetFacing;

  // How likely a room is *in this class*, where that differs from a house.
  //
  // A shop, an office and a store are in the dwelling's table at the rates a
  // house has them, which are nearly nought - and a warehouse is made of
  // exactly those three. Without this a warehouse comes out as a staircase and
  // a water closet, which is what happened.
  std::vector<std::pair<RoomUse, std::array<double, standingCount>>> existsOverride;

  // Whether the cooking chain applies - kitchen, else scullery, else living
  // room, certainly. A warehouse has none of them and must not be given one.
  bool dwelling = true;

  // Multiplies the dwelling's typical whole-building area. A tavern is bigger
  // than a house of the same standing and a lodge is smaller.
  double sizeScale = 1.;
};

struct Prior
{
  BuildingClass klass = BuildingClass::Dwelling;
  std::vector<RoomUse> vocabulary;
  std::vector<RoomUse> streetFacing;
  std::vector<PairPrior> pairs;
  bool dwelling = true;

  BayesianNetwork net;

  uint32_t standing = BayesianNetwork::invalid;
  uint32_t storeys = BayesianNetwork::invalid;
  uint32_t basement = BayesianNetwork::invalid;
  uint32_t totalArea = BayesianNetwork::invalid;
  uint32_t footprintAspect = BayesianNetwork::invalid;
  uint32_t bedroomCount = BayesianNetwork::invalid;
  uint32_t closetCount = BayesianNetwork::invalid;

  std::array<uint32_t, roomUseCount> exists{};
  std::array<uint32_t, roomUseCount> area{};
  std::array<uint32_t, roomUseCount> aspect{};

  // Per entry of `pairs`: the adjacency node and its link-type node.
  std::vector<std::array<uint32_t, 2>> pairNodes;

  std::string invalidWhy;

  bool has(const RoomUse use) const
  {
    return exists[uint32_t(use)] != BayesianNetwork::invalid;
  }
};

// Keeps a hand-authored probability off the rails without erasing the ones
// that are meant to be certainties. A house has a kitchen and a slum has no
// drawing room; both are structure, not belief, and a network that hedges
// them produces one kitchenless house in every five hundred.

// The classes, each over its own vocabulary.
//
// Only the dwelling is fitted to the corpus - it is the only class with enough
// transcribed examples. The rest are authored from the designs the book gives
// one or two of, which is what the data allows and is still a working
// generator: a hand-authored network is a generator, and a learned network
// with no data is not.
const ClassProfile &classProfile(const BuildingClass klass)
{
  static const std::vector<ClassProfile> profiles = [] {
    std::vector<ClassProfile> all;
    all.resize(size_t(BuildingClass::Count));

    // Everything a house can have. This is the dwelling's vocabulary and the
    // base most other classes take a slice of.
    const std::vector<RoomUse> house = {
      RoomUse::Kitchen, RoomUse::Scullery, RoomUse::Larder, RoomUse::Pantry,
      RoomUse::Dairy, RoomUse::Washhouse, RoomUse::Cellar,
      RoomUse::Parlour, RoomUse::LivingRoom, RoomUse::DiningRoom,
      RoomUse::DrawingRoom, RoomUse::BreakfastRoom, RoomUse::Study,
      RoomUse::Library, RoomUse::MorningRoom, RoomUse::Conservatory,
      RoomUse::Veranda, RoomUse::ServantsHall,
      RoomUse::Hall, RoomUse::Passage, RoomUse::Landing, RoomUse::Staircase,
      RoomUse::Lobby, RoomUse::Porch,
      RoomUse::Bedroom, RoomUse::DressingRoom, RoomUse::Nursery,
      RoomUse::Boudoir, RoomUse::Closet, RoomUse::WaterCloset, RoomUse::Bathroom,
      RoomUse::Office, RoomUse::Store, RoomUse::Yard,
    };

    ClassProfile dwelling;
    dwelling.klass = BuildingClass::Dwelling;
    dwelling.rooms = house;
    dwelling.rooms.push_back(RoomUse::Shop);
    dwelling.rooms.push_back(RoomUse::Workshop);
    dwelling.streetFacing = {RoomUse::Shop, RoomUse::Porch};
    all[size_t(BuildingClass::Dwelling)] = dwelling;

    // A shop with the living accommodation stacked over it. The shop is
    // certain, the polite rooms thin out, and the warehouse behind is what a
    // trade needs that a house does not.
    ClassProfile shop;
    shop.klass = BuildingClass::ShopHouse;
    shop.rooms = {
      RoomUse::Shop, RoomUse::Workshop, RoomUse::Store,
      RoomUse::Kitchen, RoomUse::Scullery, RoomUse::Pantry, RoomUse::Cellar,
      RoomUse::Parlour, RoomUse::DrawingRoom, RoomUse::LivingRoom,
      RoomUse::Hall, RoomUse::Passage, RoomUse::Landing, RoomUse::Staircase,
      RoomUse::Lobby, RoomUse::Porch,
      RoomUse::Bedroom, RoomUse::Closet, RoomUse::WaterCloset, RoomUse::Yard,
      RoomUse::Office,
    };
    shop.extraPairs = {
      {RoomUse::Shop, RoomUse::Parlour, .80, .95},
      {RoomUse::Shop, RoomUse::Office, .60, .95},
      {RoomUse::Shop, RoomUse::Store, .55, .95},
      {RoomUse::Shop, RoomUse::Workshop, .60, .90},
      {RoomUse::Shop, RoomUse::Passage, .70, .90},
      {RoomUse::Workshop, RoomUse::Yard, .55, .95},
    };
    // The shop is the building. Everything else is what fits over it.
    shop.existsOverride = {
      {RoomUse::Shop, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Store, {.55, .60, .65, .70, .70, .55}},
      {RoomUse::Workshop, {.50, .55, .50, .40, .35, .55}},
      {RoomUse::Office, {.25, .35, .45, .55, .60, .25}},
    };
    shop.streetFacing = {RoomUse::Shop, RoomUse::Workshop};
    shop.sizeScale = .95;
    all[size_t(BuildingClass::ShopHouse)] = shop;

    // A tavern. The bar is the plan: everything public is arranged so that
    // nobody can enter or leave without passing within sight of it, which the
    // book states outright and which is why the bar pairs with almost
    // everything on the ground floor.
    ClassProfile inn;
    inn.klass = BuildingClass::Inn;
    inn.rooms = {
      RoomUse::Bar, RoomUse::TapRoom, RoomUse::Parlour, RoomUse::ClubRoom,
      RoomUse::CoffeeRoom, RoomUse::BilliardRoom,
      RoomUse::Kitchen, RoomUse::Scullery, RoomUse::Larder, RoomUse::Pantry,
      RoomUse::Cellar, RoomUse::Brewhouse,
      RoomUse::Hall, RoomUse::Passage, RoomUse::Landing, RoomUse::Staircase,
      RoomUse::Lobby, RoomUse::Porch,
      RoomUse::Bedroom, RoomUse::Closet, RoomUse::WaterCloset,
      RoomUse::Yard, RoomUse::Stable, RoomUse::Store,
    };
    inn.extraPairs = {
      {RoomUse::Bar, RoomUse::TapRoom, .90, .85},
      {RoomUse::Bar, RoomUse::Parlour, .85, .90},
      {RoomUse::Bar, RoomUse::Kitchen, .75, .90},
      {RoomUse::Bar, RoomUse::Staircase, .80, .30},
      {RoomUse::Bar, RoomUse::Cellar, .60, .90},
      {RoomUse::Bar, RoomUse::Hall, .70, .60},
      {RoomUse::Bar, RoomUse::Lobby, .55, .80},
      {RoomUse::Bar, RoomUse::Store, .45, .95},
      {RoomUse::TapRoom, RoomUse::Yard, .70, .95},
      {RoomUse::Landing, RoomUse::ClubRoom, .90, 1.0},
      {RoomUse::Landing, RoomUse::CoffeeRoom, .88, 1.0},
      {RoomUse::Landing, RoomUse::BilliardRoom, .85, 1.0},
      {RoomUse::ClubRoom, RoomUse::CoffeeRoom, .35, .90},
      {RoomUse::Yard, RoomUse::Stable, .80, .95},
      {RoomUse::Yard, RoomUse::Brewhouse, .60, .95},
    };
    inn.existsOverride = {
      {RoomUse::Bar, {.98, .99, .99, .99, .99, .98}},
      {RoomUse::Yard, {.80, .85, .88, .90, .90, .92}},
      {RoomUse::Stable, {.35, .48, .60, .72, .80, .75}},
      {RoomUse::Cellar, {.85, .90, .93, .95, .95, .85}},
      {RoomUse::Store, {.45, .50, .55, .60, .62, .48}},
    };
    inn.streetFacing = {RoomUse::Bar, RoomUse::TapRoom};
    inn.sizeScale = 1.8;
    all[size_t(BuildingClass::Inn)] = inn;

    // Two school rooms and a residence under one roof. Not a dwelling as a
    // whole, but it contains one, so the cooking chain still applies.
    ClassProfile school;
    school.klass = BuildingClass::School;
    school.rooms = {
      RoomUse::SchoolRoom, RoomUse::Porch, RoomUse::Yard, RoomUse::WaterCloset,
      RoomUse::Parlour, RoomUse::Kitchen, RoomUse::Scullery, RoomUse::Pantry,
      RoomUse::Cellar, RoomUse::Store,
      RoomUse::Hall, RoomUse::Passage, RoomUse::Landing, RoomUse::Staircase,
      RoomUse::Bedroom, RoomUse::Closet,
    };
    school.extraPairs = {
      {RoomUse::SchoolRoom, RoomUse::Porch, .95, .90},
      {RoomUse::SchoolRoom, RoomUse::Yard, .85, .90},
      {RoomUse::SchoolRoom, RoomUse::Passage, .55, .95},
      {RoomUse::SchoolRoom, RoomUse::Kitchen, .30, .95},
      {RoomUse::SchoolRoom, RoomUse::Store, .40, .95},
    };
    school.existsOverride = {
      {RoomUse::SchoolRoom, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Yard, {.85, .88, .90, .92, .92, .90}},
      {RoomUse::Porch, {.85, .88, .90, .92, .92, .88}},
      {RoomUse::Store, {.40, .45, .50, .55, .60, .45}},
    };
    school.sizeScale = 2.2;
    all[size_t(BuildingClass::School)] = school;

    // Public rooms with a residence over them. The waiting room is the hub of
    // the public half and the surgeon has his own way in.
    ClassProfile dispensary;
    dispensary.klass = BuildingClass::Dispensary;
    dispensary.rooms = {
      RoomUse::Dispensary, RoomUse::WaitingRoom, RoomUse::ConsultingRoom,
      RoomUse::Office, RoomUse::Porch, RoomUse::Hall, RoomUse::Passage,
      RoomUse::Landing, RoomUse::Staircase, RoomUse::Lobby, RoomUse::WaterCloset,
      RoomUse::Kitchen, RoomUse::Scullery, RoomUse::Pantry, RoomUse::LivingRoom,
      RoomUse::Parlour, RoomUse::Bedroom, RoomUse::Closet, RoomUse::Yard,
    };
    dispensary.extraPairs = {
      {RoomUse::WaitingRoom, RoomUse::Dispensary, .90, .80},
      {RoomUse::WaitingRoom, RoomUse::ConsultingRoom, .85, 1.0},
      {RoomUse::WaitingRoom, RoomUse::Hall, .85, .90},
      {RoomUse::WaitingRoom, RoomUse::Porch, .55, .90},
      {RoomUse::ConsultingRoom, RoomUse::Dispensary, .45, 1.0},
      {RoomUse::ConsultingRoom, RoomUse::Porch, .40, 1.0},
      {RoomUse::Office, RoomUse::Hall, .70, .95},
    };
    dispensary.existsOverride = {
      {RoomUse::Dispensary, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::WaitingRoom, {.92, .93, .95, .95, .95, .90}},
      {RoomUse::ConsultingRoom, {.88, .90, .93, .95, .95, .88}},
      {RoomUse::Office, {.35, .42, .50, .58, .62, .35}},
      {RoomUse::LivingRoom, {.70, .72, .75, .75, .70, .75}},
    };
    dispensary.sizeScale = 1.5;
    all[size_t(BuildingClass::Dispensary)] = dispensary;

    // Open floors on cast iron columns. Not a dwelling, and the point of
    // having it as a class is that nothing in it should be given a bedroom.
    ClassProfile warehouse;
    warehouse.klass = BuildingClass::Warehouse;
    warehouse.rooms = {
      RoomUse::Shop, RoomUse::Office, RoomUse::Store, RoomUse::Workshop,
      RoomUse::Staircase, RoomUse::Passage, RoomUse::Cellar, RoomUse::Yard,
      RoomUse::WaterCloset, RoomUse::Loft,
    };
    warehouse.extraPairs = {
      {RoomUse::Shop, RoomUse::Office, .85, .90},
      {RoomUse::Shop, RoomUse::Store, .70, .95},
      {RoomUse::Shop, RoomUse::Staircase, .80, .40},
      {RoomUse::Store, RoomUse::Loft, .45, .95},
      {RoomUse::Store, RoomUse::Yard, .60, .95},
    };
    // A warehouse is made of the rooms a house barely has, so nearly all of
    // its existences are overridden.
    warehouse.existsOverride = {
      {RoomUse::Shop, {.92, .92, .92, .92, .92, .85}},
      {RoomUse::Office, {.85, .88, .90, .92, .92, .80}},
      {RoomUse::Store, {.90, .92, .94, .95, .95, .90}},
      {RoomUse::Workshop, {.45, .45, .40, .35, .30, .50}},
      {RoomUse::Loft, {.55, .60, .62, .65, .65, .60}},
      {RoomUse::Cellar, {.45, .50, .55, .60, .65, .40}},
      {RoomUse::Staircase, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Passage, {.60, .62, .65, .68, .70, .60}},
      {RoomUse::Yard, {.70, .72, .74, .75, .75, .80}},
      {RoomUse::WaterCloset, {.75, .80, .85, .88, .90, .70}},
    };
    warehouse.dwelling = false;
    warehouse.sizeScale = 2.6;
    all[size_t(BuildingClass::Warehouse)] = warehouse;

    // A house with a range of offices, and working buildings round a yard.
    ClassProfile farm;
    farm.klass = BuildingClass::Farmstead;
    farm.rooms = house;
    farm.rooms.push_back(RoomUse::Brewhouse);
    farm.rooms.push_back(RoomUse::Stable);
    farm.rooms.push_back(RoomUse::Barn);
    farm.rooms.push_back(RoomUse::Loft);
    farm.extraPairs = {
      {RoomUse::Yard, RoomUse::Stable, .85, .95},
      {RoomUse::Yard, RoomUse::Barn, .75, .95},
      {RoomUse::Yard, RoomUse::Brewhouse, .70, .95},
      {RoomUse::Stable, RoomUse::Loft, .55, .90},
      {RoomUse::Barn, RoomUse::Loft, .45, .90},
      {RoomUse::Brewhouse, RoomUse::Dairy, .45, 1.0},
      {RoomUse::Office, RoomUse::Yard, .35, .95},
    };
    // A farmstead is a house and its working buildings, and the working
    // buildings are what make it one.
    farm.existsOverride = {
      {RoomUse::Yard, {.90, .92, .94, .95, .95, .98}},
      {RoomUse::Dairy, {.55, .65, .72, .78, .80, .88}},
      {RoomUse::Stable, {.55, .65, .75, .82, .88, .90}},
      {RoomUse::Barn, {.40, .50, .60, .68, .75, .85}},
      {RoomUse::Brewhouse, {.25, .35, .45, .55, .60, .65}},
      {RoomUse::Loft, {.35, .45, .52, .58, .62, .70}},
      {RoomUse::Washhouse, {.45, .55, .62, .68, .72, .75}},
      {RoomUse::Office, {.20, .28, .38, .48, .55, .45}},
    };
    farm.sizeScale = 1.3;
    all[size_t(BuildingClass::Farmstead)] = farm;

    // --- ecclesiastical ---------------------------------------------------
    //
    // A parish church. The plan is one division - nave from chancel, the
    // people from the priest - and everything else hangs off it. The aisles
    // flank the nave, the transepts cross it, the vestry opens off the
    // chancel because that is who uses it, and the tower stands at the west
    // end where the people come in.
    ClassProfile church;
    church.klass = BuildingClass::Church;
    church.rooms = {
      RoomUse::Nave, RoomUse::Chancel, RoomUse::Aisle, RoomUse::Transept,
      RoomUse::Sanctuary, RoomUse::Narthex, RoomUse::SideChapel,
      RoomUse::Vestry, RoomUse::Crypt, RoomUse::Belfry, RoomUse::Tower,
      RoomUse::Porch, RoomUse::Gallery, RoomUse::Staircase, RoomUse::Landing, RoomUse::Store,
    };
    church.extraPairs = {
      {RoomUse::Nave, RoomUse::Chancel, .98, .25},
      {RoomUse::Nave, RoomUse::Aisle, .95, .10},
      {RoomUse::Nave, RoomUse::Transept, .92, .15},
      {RoomUse::Nave, RoomUse::Narthex, .85, .60},
      {RoomUse::Nave, RoomUse::Porch, .80, .90},
      {RoomUse::Nave, RoomUse::Tower, .65, .70},
      {RoomUse::Nave, RoomUse::Gallery, .55, .40},
      {RoomUse::Chancel, RoomUse::Sanctuary, .92, .20},
      {RoomUse::Chancel, RoomUse::Vestry, .88, .95},
      {RoomUse::Chancel, RoomUse::SideChapel, .60, .35},
      {RoomUse::Transept, RoomUse::SideChapel, .55, .30},
      {RoomUse::Aisle, RoomUse::Porch, .55, .90},
      {RoomUse::Tower, RoomUse::Belfry, .92, .60},
      {RoomUse::Tower, RoomUse::Staircase, .88, .50},
      {RoomUse::Staircase, RoomUse::Gallery, .85, .50},
      {RoomUse::Staircase, RoomUse::Crypt, .60, .90},
      {RoomUse::Vestry, RoomUse::Store, .45, .95},
    };
    church.existsOverride = {
      {RoomUse::Nave, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Porch, {.70, .75, .82, .88, .92, .80}},
      {RoomUse::Staircase, {.55, .62, .74, .85, .92, .58}},
      {RoomUse::Store, {.25, .30, .38, .45, .50, .28}},
    };
    church.dwelling = false;
    church.sizeScale = 3.4;
    all[size_t(BuildingClass::Church)] = church;

    // A dissenting chapel, which the book specifies beside the Episcopal one:
    // a preaching box with galleries round it and a vestry behind, and no
    // chancel at all, because nothing happens at the east end that the
    // congregation is not meant to see.
    ClassProfile chapel;
    chapel.klass = BuildingClass::Chapel;
    chapel.rooms = {
      RoomUse::Nave, RoomUse::Gallery, RoomUse::Narthex, RoomUse::Vestry,
      RoomUse::SchoolRoom, RoomUse::Porch, RoomUse::Staircase, RoomUse::Landing,
      RoomUse::Store, RoomUse::WaterCloset, RoomUse::Yard,
    };
    chapel.extraPairs = {
      {RoomUse::Nave, RoomUse::Gallery, .85, .30},
      {RoomUse::Nave, RoomUse::Narthex, .90, .70},
      {RoomUse::Nave, RoomUse::Vestry, .82, .95},
      {RoomUse::Nave, RoomUse::Porch, .70, .90},
      {RoomUse::Narthex, RoomUse::Staircase, .85, .40},
      {RoomUse::Staircase, RoomUse::Gallery, .92, .40},
      {RoomUse::Vestry, RoomUse::SchoolRoom, .55, .95},
      {RoomUse::SchoolRoom, RoomUse::Yard, .50, .90},
      {RoomUse::Yard, RoomUse::WaterCloset, .70, 1.0},
    };
    chapel.existsOverride = {
      {RoomUse::Nave, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Gallery, {.55, .65, .75, .82, .85, .55}},
      {RoomUse::Vestry, {.85, .88, .92, .94, .95, .85}},
      {RoomUse::SchoolRoom, {.30, .40, .55, .65, .70, .35}},
      {RoomUse::Staircase, {.60, .70, .80, .86, .88, .60}},
    };
    chapel.dwelling = false;
    chapel.sizeScale = 2.2;
    all[size_t(BuildingClass::Chapel)] = chapel;

    // A temple: a sanctuary approached through a porch, standing in its own
    // court, with a treasury and a crypt under it. Older and more axial than
    // a church - there is no nave full of seated people, there is a way in and
    // a thing at the end of it.
    ClassProfile temple;
    temple.klass = BuildingClass::Temple;
    temple.rooms = {
      RoomUse::Sanctuary, RoomUse::Nave, RoomUse::Narthex, RoomUse::Porch,
      RoomUse::Courtyard, RoomUse::Cloister, RoomUse::Crypt, RoomUse::Store,
      RoomUse::Vestry, RoomUse::Tower, RoomUse::Staircase, RoomUse::SideChapel,
    };
    temple.extraPairs = {
      {RoomUse::Courtyard, RoomUse::Porch, .92, .60},
      {RoomUse::Porch, RoomUse::Narthex, .85, .50},
      {RoomUse::Narthex, RoomUse::Nave, .92, .30},
      {RoomUse::Nave, RoomUse::Sanctuary, .95, .35},
      {RoomUse::Sanctuary, RoomUse::Store, .65, .95},
      {RoomUse::Sanctuary, RoomUse::SideChapel, .50, .40},
      {RoomUse::Courtyard, RoomUse::Cloister, .80, .25},
      {RoomUse::Cloister, RoomUse::Vestry, .60, .90},
      {RoomUse::Staircase, RoomUse::Crypt, .75, .85},
      {RoomUse::Staircase, RoomUse::Tower, .70, .60},
      {RoomUse::Nave, RoomUse::Staircase, .55, .70},
    };
    temple.existsOverride = {
      {RoomUse::Sanctuary, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Courtyard, {.70, .78, .85, .90, .95, .80}},
      {RoomUse::Cloister, {.20, .30, .45, .62, .78, .30}},
      {RoomUse::Crypt, {.25, .35, .48, .62, .75, .30}},
      {RoomUse::Store, {.55, .62, .70, .78, .82, .58}},
      {RoomUse::Staircase, {.50, .58, .70, .80, .88, .52}},
    };
    temple.dwelling = false;
    temple.sizeScale = 3.0;
    all[size_t(BuildingClass::Temple)] = temple;

    // --- medieval and defensive ------------------------------------------
    //
    // A castle is a wall with buildings against it, and the courtyard is the
    // circulation: almost everything opens onto the bailey rather than onto
    // the next room, which is the opposite of a house and is why the pair
    // table is so much flatter here.
    ClassProfile castle;
    castle.klass = BuildingClass::Castle;
    castle.rooms = {
      RoomUse::Courtyard, RoomUse::Gatehouse, RoomUse::Guardroom, RoomUse::Keep,
      RoomUse::GreatHall, RoomUse::ScreensPassage, RoomUse::Solar,
      RoomUse::Buttery, RoomUse::Pantry, RoomUse::Kitchen, RoomUse::Scullery,
      RoomUse::Larder, RoomUse::Cellar, RoomUse::Barracks, RoomUse::Armoury,
      RoomUse::Dungeon, RoomUse::Tower, RoomUse::SideChapel,
      RoomUse::Stable, RoomUse::Store, RoomUse::Staircase, RoomUse::Landing, RoomUse::Bedroom,
      RoomUse::WaterCloset, RoomUse::Well,
    };
    castle.extraPairs = {
      {RoomUse::Gatehouse, RoomUse::Courtyard, .95, .40},
      {RoomUse::Gatehouse, RoomUse::Guardroom, .92, .90},
      {RoomUse::Courtyard, RoomUse::GreatHall, .90, .90},
      {RoomUse::Courtyard, RoomUse::Keep, .85, .90},
      {RoomUse::Courtyard, RoomUse::Stable, .85, .90},
      {RoomUse::Courtyard, RoomUse::Kitchen, .70, .90},
      {RoomUse::Courtyard, RoomUse::SideChapel, .65, .90},
      {RoomUse::Courtyard, RoomUse::Barracks, .70, .90},
      {RoomUse::Courtyard, RoomUse::Armoury, .55, .90},
      {RoomUse::Courtyard, RoomUse::Store, .60, .90},
      {RoomUse::Courtyard, RoomUse::Tower, .75, .80},
      {RoomUse::GreatHall, RoomUse::ScreensPassage, .92, .20},
      {RoomUse::ScreensPassage, RoomUse::Buttery, .90, .85},
      {RoomUse::ScreensPassage, RoomUse::Pantry, .88, .85},
      {RoomUse::ScreensPassage, RoomUse::Kitchen, .70, .90},
      {RoomUse::GreatHall, RoomUse::Solar, .85, .95},
      {RoomUse::GreatHall, RoomUse::Staircase, .75, .40},
      {RoomUse::Keep, RoomUse::Staircase, .88, .50},
      {RoomUse::Staircase, RoomUse::Dungeon, .60, .90},
      {RoomUse::Staircase, RoomUse::Barracks, .55, .85},
      {RoomUse::Solar, RoomUse::Bedroom, .70, .95},
      {RoomUse::Solar, RoomUse::WaterCloset, .55, .95},
      {RoomUse::Kitchen, RoomUse::Well, .45, .60},
      {RoomUse::Courtyard, RoomUse::Well, .80, .30},
    };
    castle.existsOverride = {
      {RoomUse::Courtyard, {.85, .88, .92, .95, .98, .88}},
      {RoomUse::Gatehouse, {.75, .82, .88, .93, .96, .70}},
      {RoomUse::GreatHall, {.80, .85, .92, .96, .98, .82}},
      {RoomUse::Keep, {.65, .72, .80, .88, .94, .62}},
      {RoomUse::Well, {.60, .68, .78, .85, .90, .70}},
      {RoomUse::Stable, {.55, .65, .75, .85, .90, .75}},
      {RoomUse::Bedroom, {.55, .62, .72, .82, .90, .58}},
    };
    castle.sizeScale = 5.0;
    all[size_t(BuildingClass::Castle)] = castle;

    // A medieval manor house: the same great hall without the wall round it.
    // Pantin's survey is the corpus for this one - hall, solar, screens
    // passage, buttery, pantry, parlour, chamber, undercroft, courtyard.
    ClassProfile manor;
    manor.klass = BuildingClass::ManorHouse;
    manor.rooms = {
      RoomUse::GreatHall, RoomUse::ScreensPassage, RoomUse::Solar,
      RoomUse::Buttery, RoomUse::Pantry, RoomUse::Kitchen, RoomUse::Scullery,
      RoomUse::Larder, RoomUse::Cellar, RoomUse::Parlour, RoomUse::Bedroom,
      RoomUse::SideChapel, RoomUse::Courtyard, RoomUse::Gallery,
      RoomUse::Staircase, RoomUse::Landing, RoomUse::Porch, RoomUse::Passage, RoomUse::Store,
      RoomUse::WaterCloset, RoomUse::Stable, RoomUse::Yard,
    };
    manor.extraPairs = {
      {RoomUse::Porch, RoomUse::ScreensPassage, .88, .85},
      {RoomUse::ScreensPassage, RoomUse::GreatHall, .95, .20},
      {RoomUse::ScreensPassage, RoomUse::Buttery, .90, .85},
      {RoomUse::ScreensPassage, RoomUse::Pantry, .88, .85},
      {RoomUse::ScreensPassage, RoomUse::Kitchen, .65, .90},
      {RoomUse::GreatHall, RoomUse::Solar, .88, .95},
      {RoomUse::GreatHall, RoomUse::Parlour, .70, .95},
      {RoomUse::GreatHall, RoomUse::Staircase, .70, .40},
      {RoomUse::GreatHall, RoomUse::Courtyard, .55, .90},
      {RoomUse::Solar, RoomUse::Bedroom, .72, .95},
      {RoomUse::Solar, RoomUse::SideChapel, .40, .90},
      {RoomUse::Staircase, RoomUse::Solar, .82, .60},
      {RoomUse::Staircase, RoomUse::Gallery, .70, .50},
      {RoomUse::Courtyard, RoomUse::Kitchen, .60, .90},
      {RoomUse::Courtyard, RoomUse::Stable, .75, .90},
      {RoomUse::Shop, RoomUse::ScreensPassage, .45, .90},
    };
    manor.existsOverride = {
      {RoomUse::GreatHall, {.90, .92, .95, .97, .98, .95}},
      {RoomUse::ScreensPassage, {.80, .84, .90, .94, .96, .85}},
      {RoomUse::Solar, {.75, .80, .88, .93, .96, .80}},
      {RoomUse::Buttery, {.70, .76, .85, .90, .94, .78}},
      {RoomUse::Courtyard, {.30, .40, .55, .70, .85, .45}},
      {RoomUse::Shop, {.35, .40, .35, .20, .08, .10}},
      {RoomUse::Gallery, {.05, .10, .22, .42, .65, .12}},
    };
    manor.sizeScale = 2.4;
    all[size_t(BuildingClass::ManorHouse)] = manor;

    // --- institutional ----------------------------------------------------
    //
    // A prison is a gallery with cells off it and a yard to walk in. The
    // governor's house and the chapel are attached, which is the only part of
    // it that resembles an ordinary building.
    ClassProfile prison;
    prison.klass = BuildingClass::Prison;
    prison.rooms = {
      RoomUse::Cell, RoomUse::Gallery, RoomUse::ExerciseYard, RoomUse::Guardroom,
      RoomUse::Infirmary, RoomUse::SideChapel, RoomUse::Workshop,
      RoomUse::Washhouse, RoomUse::Kitchen, RoomUse::Scullery, RoomUse::Store,
      RoomUse::Office, RoomUse::Staircase, RoomUse::Landing, RoomUse::Passage, RoomUse::Porch,
      RoomUse::WaterCloset, RoomUse::Parlour, RoomUse::Bedroom, RoomUse::Yard,
    };
    prison.extraPairs = {
      {RoomUse::Gallery, RoomUse::Cell, .98, 1.0},
      {RoomUse::Gallery, RoomUse::Staircase, .92, .40},
      {RoomUse::Gallery, RoomUse::Guardroom, .90, .95},
      {RoomUse::Gallery, RoomUse::ExerciseYard, .80, .95},
      {RoomUse::Gallery, RoomUse::SideChapel, .65, .95},
      {RoomUse::Gallery, RoomUse::Infirmary, .60, .95},
      {RoomUse::Gallery, RoomUse::Workshop, .55, .95},
      {RoomUse::Guardroom, RoomUse::Porch, .85, .95},
      {RoomUse::Guardroom, RoomUse::Office, .70, .95},
      {RoomUse::ExerciseYard, RoomUse::Washhouse, .45, .95},
      {RoomUse::Kitchen, RoomUse::Gallery, .55, .95},
      {RoomUse::Office, RoomUse::Parlour, .50, .95},
    };
    prison.existsOverride = {
      {RoomUse::Cell, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Gallery, {.95, .96, .97, .98, .98, .95}},
      {RoomUse::ExerciseYard, {.85, .88, .92, .94, .95, .88}},
      {RoomUse::Guardroom, {.88, .90, .93, .95, .96, .88}},
      {RoomUse::Workshop, {.45, .55, .68, .78, .85, .50}},
      {RoomUse::Washhouse, {.40, .50, .62, .72, .80, .45}},
      {RoomUse::Office, {.60, .68, .78, .85, .90, .60}},
      {RoomUse::Bedroom, {.55, .62, .72, .80, .85, .58}},
      {RoomUse::Parlour, {.45, .55, .68, .78, .85, .50}},
    };
    prison.sizeScale = 4.0;
    all[size_t(BuildingClass::Prison)] = prison;

    // A mansion. Not a large house: it has state rooms a house does not, and
    // a service establishment run as a household of its own - a housekeeper's
    // room, a butler's room, a still room and a servants' hall, which is the
    // part that makes it a different building rather than a bigger one.
    ClassProfile mansion;
    mansion.klass = BuildingClass::Mansion;
    mansion.rooms = house;
    for (const RoomUse extra : {RoomUse::Saloon, RoomUse::Ballroom,
                                RoomUse::SmokingRoom, RoomUse::GunRoom,
                                RoomUse::StillRoom, RoomUse::HousekeepersRoom,
                                RoomUse::ButlersRoom, RoomUse::Gallery,
                                RoomUse::SideChapel, RoomUse::Stable,
                                RoomUse::Courtyard, RoomUse::Tower})
    {
      mansion.rooms.push_back(extra);
    }
    mansion.extraPairs = {
      {RoomUse::Hall, RoomUse::Saloon, .92, .25},
      {RoomUse::Saloon, RoomUse::DrawingRoom, .88, .40},
      {RoomUse::Saloon, RoomUse::DiningRoom, .85, .55},
      {RoomUse::Saloon, RoomUse::Ballroom, .70, .35},
      {RoomUse::Saloon, RoomUse::Library, .60, .85},
      {RoomUse::Saloon, RoomUse::Staircase, .75, .30},
      {RoomUse::DrawingRoom, RoomUse::Ballroom, .55, .45},
      {RoomUse::Library, RoomUse::SmokingRoom, .55, .90},
      {RoomUse::SmokingRoom, RoomUse::GunRoom, .45, .90},
      {RoomUse::DiningRoom, RoomUse::ButlersRoom, .60, .95},
      {RoomUse::ButlersRoom, RoomUse::Pantry, .80, .90},
      {RoomUse::ButlersRoom, RoomUse::Cellar, .55, .95},
      {RoomUse::HousekeepersRoom, RoomUse::StillRoom, .75, .90},
      {RoomUse::HousekeepersRoom, RoomUse::ServantsHall, .70, .95},
      {RoomUse::HousekeepersRoom, RoomUse::Store, .65, .95},
      {RoomUse::ServantsHall, RoomUse::Kitchen, .75, .95},
      {RoomUse::StillRoom, RoomUse::Kitchen, .55, .95},
      {RoomUse::Gallery, RoomUse::Landing, .80, .35},
      {RoomUse::Gallery, RoomUse::Bedroom, .55, 1.0},
      {RoomUse::Courtyard, RoomUse::Stable, .80, .90},
      {RoomUse::Courtyard, RoomUse::Kitchen, .45, .90},
      {RoomUse::Hall, RoomUse::SideChapel, .40, .90},
      {RoomUse::Staircase, RoomUse::Tower, .50, .70},
    };
    mansion.existsOverride = {
      {RoomUse::Saloon, {.00, .00, .20, .70, .95, .30}},
      {RoomUse::Ballroom, {.00, .00, .05, .35, .78, .05}},
      {RoomUse::SmokingRoom, {.00, .00, .15, .55, .82, .15}},
      {RoomUse::GunRoom, {.00, .00, .08, .35, .62, .30}},
      {RoomUse::StillRoom, {.00, .00, .18, .58, .85, .20}},
      {RoomUse::HousekeepersRoom, {.00, .00, .22, .68, .92, .22}},
      {RoomUse::ButlersRoom, {.00, .00, .25, .72, .93, .22}},
      {RoomUse::ServantsHall, {.00, .05, .30, .72, .94, .28}},
      {RoomUse::Gallery, {.00, .00, .10, .42, .75, .10}},
      {RoomUse::Library, {.05, .12, .35, .72, .92, .20}},
      {RoomUse::Stable, {.05, .12, .30, .60, .82, .55}},
      {RoomUse::Courtyard, {.00, .02, .10, .32, .60, .20}},
      {RoomUse::Tower, {.00, .00, .03, .15, .38, .05}},
      {RoomUse::Conservatory, {.00, .02, .18, .55, .80, .12}},
    };
    mansion.sizeScale = 3.2;
    all[size_t(BuildingClass::Mansion)] = mansion;

    // --- trade -----------------------------------------------------------
    //
    // A guild hall. One great room over a vaulted undercroft, with a council
    // chamber above for the business the whole company is not party to, a
    // kitchen for the feasts, and the undercroft let out as shops to the
    // street - which is how the building pays for itself.
    ClassProfile guild;
    guild.klass = BuildingClass::GuildHall;
    guild.rooms = {
      RoomUse::GreatHall, RoomUse::CouncilChamber, RoomUse::Undercroft,
      RoomUse::Shop, RoomUse::ScreensPassage, RoomUse::Buttery, RoomUse::Pantry,
      RoomUse::Kitchen, RoomUse::Scullery, RoomUse::Cellar, RoomUse::StrongRoom,
      RoomUse::CountingHouse, RoomUse::SideChapel, RoomUse::Gallery,
      RoomUse::Porch, RoomUse::Staircase, RoomUse::Landing, RoomUse::Passage, RoomUse::Store,
      RoomUse::WaterCloset, RoomUse::Courtyard, RoomUse::Parlour,
    };
    guild.extraPairs = {
      {RoomUse::Porch, RoomUse::ScreensPassage, .88, .85},
      {RoomUse::ScreensPassage, RoomUse::GreatHall, .95, .20},
      {RoomUse::ScreensPassage, RoomUse::Buttery, .85, .85},
      {RoomUse::ScreensPassage, RoomUse::Pantry, .82, .85},
      {RoomUse::ScreensPassage, RoomUse::Kitchen, .70, .90},
      {RoomUse::GreatHall, RoomUse::Staircase, .85, .40},
      {RoomUse::GreatHall, RoomUse::Parlour, .60, .95},
      {RoomUse::GreatHall, RoomUse::SideChapel, .45, .90},
      {RoomUse::Staircase, RoomUse::CouncilChamber, .90, .95},
      {RoomUse::Staircase, RoomUse::Gallery, .70, .50},
      {RoomUse::Staircase, RoomUse::Undercroft, .75, .90},
      {RoomUse::CouncilChamber, RoomUse::StrongRoom, .65, 1.0},
      {RoomUse::CouncilChamber, RoomUse::CountingHouse, .55, .95},
      {RoomUse::Undercroft, RoomUse::Shop, .85, .90},
      {RoomUse::Undercroft, RoomUse::Cellar, .60, .90},
      {RoomUse::Undercroft, RoomUse::Store, .55, .95},
      {RoomUse::Courtyard, RoomUse::Kitchen, .55, .90},
    };
    guild.existsOverride = {
      {RoomUse::GreatHall, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::CouncilChamber, {.70, .78, .86, .92, .95, .65}},
      {RoomUse::Undercroft, {.55, .65, .78, .86, .92, .45}},
      {RoomUse::Shop, {.45, .55, .68, .78, .85, .30}},
      {RoomUse::StrongRoom, {.45, .55, .70, .82, .90, .35}},
      {RoomUse::CountingHouse, {.35, .45, .60, .72, .80, .28}},
      {RoomUse::Staircase, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Porch, {.70, .76, .84, .90, .93, .68}},
    };
    guild.streetFacing = {RoomUse::Shop, RoomUse::Undercroft, RoomUse::Porch};
    guild.dwelling = false;
    guild.sizeScale = 2.8;
    all[size_t(BuildingClass::GuildHall)] = guild;

    // --- the two ends of the medieval scale ------------------------------
    //
    // The peasant's longhouse. One range under one roof: the house-place with
    // the hearth in it, the byre at the lower end with the floor falling away
    // from the living end, and a cross passage between them with a door at
    // each side. There is no kitchen - the house-place *is* the kitchen, and
    // it is also the dining room, the parlour and, for most of the household,
    // the bedroom. A second room comes with a little money: an inner room for
    // the master's bed. A loft over one end comes with a little more.
    //
    // The outdoor ground is part of the programme and not scenery. The croft
    // is worked, the pen holds the stock that does not winter indoors, and the
    // midden is where the byre is mucked out to - and their positions relative
    // to the house and to each other are the whole layout of a farmstead.
    ClassProfile cottage;
    cottage.klass = BuildingClass::PeasantCottage;
    cottage.rooms = {
      RoomUse::LivingRoom, RoomUse::Byre, RoomUse::ScreensPassage, RoomUse::Bedroom,
      RoomUse::Loft, RoomUse::Staircase, RoomUse::Landing, RoomUse::Pantry, RoomUse::Store, RoomUse::Yard, RoomUse::Croft,
      RoomUse::Pen, RoomUse::Midden, RoomUse::Well, RoomUse::Barn, RoomUse::Workshop,
    };
    cottage.extraPairs = {
      {RoomUse::ScreensPassage, RoomUse::LivingRoom, .92, .35},
      {RoomUse::ScreensPassage, RoomUse::Byre, .90, .45},
      {RoomUse::ScreensPassage, RoomUse::Yard, .85, .60},
      {RoomUse::LivingRoom, RoomUse::Bedroom, .88, .25},
      {RoomUse::LivingRoom, RoomUse::Loft, .70, .30},
      {RoomUse::LivingRoom, RoomUse::Pantry, .55, .70},
      {RoomUse::LivingRoom, RoomUse::Yard, .70, .90},
      {RoomUse::Byre, RoomUse::Midden, .75, 1.0},
      {RoomUse::Byre, RoomUse::Pen, .60, .95},
      {RoomUse::Yard, RoomUse::Croft, .80, .80},
      {RoomUse::Yard, RoomUse::Well, .65, .95},
      {RoomUse::Yard, RoomUse::Barn, .55, .95},
      {RoomUse::Yard, RoomUse::Pen, .60, .90},
    };
    cottage.existsOverride = {
      {RoomUse::LivingRoom, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::ScreensPassage, {.35, .62, .72, .80, .85, .70}},
      {RoomUse::Byre, {.30, .72, .55, .35, .20, .82}},
      {RoomUse::Bedroom, {.15, .45, .65, .82, .90, .50}},
      {RoomUse::Loft, {.20, .45, .58, .70, .78, .50}},
      {RoomUse::Pantry, {.08, .25, .40, .58, .70, .30}},
      {RoomUse::Store, {.20, .40, .50, .60, .68, .45}},
      {RoomUse::Yard, {.70, .92, .90, .88, .85, .95}},
      {RoomUse::Croft, {.35, .85, .60, .45, .35, .92}},
      {RoomUse::Pen, {.12, .55, .35, .25, .18, .70}},
      {RoomUse::Midden, {.40, .70, .50, .30, .15, .78}},
      {RoomUse::Well, {.10, .45, .35, .35, .40, .60}},
      {RoomUse::Barn, {.02, .40, .18, .12, .10, .62}},
      {RoomUse::Workshop, {.08, .20, .25, .22, .18, .25}},
      // Everything a cottage does not have, said outright rather than left to
      // the house table to give it at a few per cent each - which is how a
      // labourer's cottage ends up with a dressing room.
      {RoomUse::Kitchen, {.00, .02, .08, .22, .35, .05}},
      {RoomUse::Scullery, {.00, .02, .06, .15, .25, .05}},
      {RoomUse::Parlour, {.00, .05, .18, .40, .60, .10}},
      {RoomUse::Staircase, {.05, .12, .20, .35, .45, .15}},
      {RoomUse::WaterCloset, {.00, .00, .02, .08, .15, .02}},
    };
    cottage.sizeScale = .55;
    all[size_t(BuildingClass::PeasantCottage)] = cottage;

    // A lodging house. Not a large dwelling: a building with no household in
    // it at all, which is why the house table cannot describe one. Rooms of
    // beds, a room to eat in, one kitchen serving all of them, and a keeper.
    ClassProfile lodging;
    lodging.klass = BuildingClass::Dormitory;
    lodging.rooms = {
      RoomUse::Dormitory, RoomUse::Refectory, RoomUse::Kitchen, RoomUse::Scullery,
      RoomUse::Washhouse, RoomUse::Store, RoomUse::Cellar, RoomUse::Staircase,
      RoomUse::Passage, RoomUse::Landing, RoomUse::Hall, RoomUse::WaterCloset,
      RoomUse::Bedroom, RoomUse::Office, RoomUse::Yard, RoomUse::Well,
    };
    lodging.extraPairs = {
      {RoomUse::Hall, RoomUse::Refectory, .88, .40},
      {RoomUse::Hall, RoomUse::Staircase, .90, .35},
      {RoomUse::Hall, RoomUse::Office, .70, .85},
      {RoomUse::Refectory, RoomUse::Kitchen, .92, .60},
      {RoomUse::Kitchen, RoomUse::Scullery, .85, .55},
      {RoomUse::Kitchen, RoomUse::Store, .70, .90},
      {RoomUse::Landing, RoomUse::Dormitory, .95, .90},
      {RoomUse::Staircase, RoomUse::Landing, .95, .30},
      {RoomUse::Yard, RoomUse::Washhouse, .75, .90},
      {RoomUse::Yard, RoomUse::WaterCloset, .70, .95},
      {RoomUse::Yard, RoomUse::Well, .65, .95},
      {RoomUse::Office, RoomUse::Bedroom, .60, .90},
    };
    lodging.existsOverride = {
      {RoomUse::Dormitory, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Refectory, {.70, .75, .85, .92, .95, .70}},
      {RoomUse::Kitchen, {.85, .88, .92, .96, .98, .85}},
      {RoomUse::Washhouse, {.45, .55, .68, .80, .88, .55}},
      {RoomUse::Staircase, {.90, .92, .95, .97, .98, .88}},
      {RoomUse::Office, {.35, .45, .60, .72, .80, .35}},
      {RoomUse::Bedroom, {.55, .62, .72, .80, .85, .55}},
      {RoomUse::Yard, {.70, .78, .82, .85, .88, .88}},
      // A lodging house has no drawing room and no nursery.
      {RoomUse::DrawingRoom, {.00, .00, .00, .02, .05, .00}},
      {RoomUse::Nursery, {.00, .00, .00, .00, .00, .00}},
      {RoomUse::Parlour, {.05, .08, .15, .25, .35, .08}},
      {RoomUse::DiningRoom, {.00, .02, .05, .10, .15, .02}},
    };
    lodging.dwelling = false;
    lodging.sizeScale = 2.1;
    all[size_t(BuildingClass::Dormitory)] = lodging;

    // A merchant's house, which is a business with a household on top of it.
    //
    // The trading floor is on the street and open to it - the counting house
    // and the shop take the frontage, and the window is a shutter that drops
    // to make a counter. The merchant's own hall and parlour are over that,
    // and he sleeps above those, so the building is stratified by what it is
    // for rather than by who may go where: the public are on the street floor,
    // the business is entertained on the middle one, and the family is at the
    // top. The strong room is next to the counting house and the goods are in
    // the undercroft under both.
    ClassProfile commerce;
    commerce.klass = BuildingClass::CommerceHouse;
    commerce.rooms = {
      RoomUse::Shop, RoomUse::CountingHouse, RoomUse::StrongRoom,
      RoomUse::Undercroft, RoomUse::Store, RoomUse::Office,
      RoomUse::Hall, RoomUse::Parlour, RoomUse::DiningRoom, RoomUse::Solar,
      RoomUse::Kitchen, RoomUse::Scullery, RoomUse::Larder, RoomUse::Pantry,
      RoomUse::Cellar, RoomUse::Bedroom, RoomUse::Closet, RoomUse::Passage,
      RoomUse::Landing, RoomUse::Staircase, RoomUse::Lobby, RoomUse::Porch,
      RoomUse::WaterCloset, RoomUse::Yard, RoomUse::Courtyard, RoomUse::Gallery,
    };
    commerce.extraPairs = {
      // The street floor: trade, and the way past it into the house.
      {RoomUse::Shop, RoomUse::CountingHouse, .88, .85},
      {RoomUse::Shop, RoomUse::Passage, .75, .90},
      {RoomUse::Shop, RoomUse::Store, .60, .95},
      {RoomUse::CountingHouse, RoomUse::StrongRoom, .80, 1.0},
      {RoomUse::CountingHouse, RoomUse::Office, .55, .95},
      {RoomUse::CountingHouse, RoomUse::Passage, .70, .90},
      {RoomUse::CountingHouse, RoomUse::Staircase, .65, .60},
      {RoomUse::Passage, RoomUse::Staircase, .88, .35},
      {RoomUse::Passage, RoomUse::Courtyard, .70, .90},
      {RoomUse::Courtyard, RoomUse::Store, .75, .90},
      {RoomUse::Courtyard, RoomUse::Kitchen, .60, .90},
      {RoomUse::Staircase, RoomUse::Undercroft, .70, .90},
      {RoomUse::Undercroft, RoomUse::Store, .60, .85},
      // The floor above: the merchant's own rooms.
      {RoomUse::Landing, RoomUse::Solar, .88, .95},
      {RoomUse::Landing, RoomUse::Parlour, .80, .95},
      {RoomUse::Landing, RoomUse::DiningRoom, .70, .95},
      {RoomUse::Solar, RoomUse::Parlour, .55, .90},
      {RoomUse::Solar, RoomUse::Bedroom, .60, .95},
      {RoomUse::Gallery, RoomUse::Solar, .45, .50},
    };
    commerce.existsOverride = {
      {RoomUse::Shop, {.85, .88, .90, .88, .82, .70}},
      {RoomUse::CountingHouse, {.80, .88, .93, .95, .96, .70}},
      {RoomUse::StrongRoom, {.35, .50, .68, .82, .90, .30}},
      {RoomUse::Undercroft, {.40, .52, .66, .78, .86, .35}},
      {RoomUse::Store, {.60, .68, .78, .85, .90, .55}},
      {RoomUse::Solar, {.55, .65, .78, .88, .93, .55}},
      {RoomUse::Courtyard, {.25, .35, .50, .65, .78, .30}},
      {RoomUse::Office, {.35, .45, .58, .70, .78, .30}},
      {RoomUse::Staircase, {1., 1., 1., 1., 1., 1.}},
      {RoomUse::Gallery, {.05, .10, .20, .35, .52, .08}},
    };
    // The window that makes it a commerce house: the trading rooms take the
    // frontage and nothing else may.
    commerce.streetFacing = {RoomUse::Shop, RoomUse::CountingHouse,
                             RoomUse::Undercroft, RoomUse::Porch};
    commerce.sizeScale = 1.4;
    all[size_t(BuildingClass::CommerceHouse)] = commerce;

    return all;
  }();

  return profiles[size_t(klass) < profiles.size() ? size_t(klass) : 0];
}

double clampProbability(const double p)
{
  if (p >= 1.)
  {
    return 1.;
  }

  if (p <= 0.)
  {
    return 0.;
  }

  return std::min(std::max(p, .002), .998);
}

// p(this room exists), given the standing and whatever else the node hangs
// off, as a two-column row.
std::vector<double> yesNo(const double p)
{
  const double q = clampProbability(p);

  return {1. - q, q};
}

Prior buildPrior(const BuildingClass klass)
{
  const ClassProfile &profile = classProfile(klass);

  Prior p;
  p.klass = klass;
  p.vocabulary = profile.rooms;
  p.streetFacing = profile.streetFacing;
  p.dwelling = profile.dwelling;
  BayesianNetwork &net = p.net;

  // The dwelling's pairs, less any naming a room this class does not have,
  // plus the ones peculiar to it.
  const auto inVocabulary = [&profile](const RoomUse use) {
    return std::find(profile.rooms.begin(), profile.rooms.end(), use) != profile.rooms.end();
  };

  for (const PairPrior &pair : pairPriors())
  {
    if (inVocabulary(pair.a) && inVocabulary(pair.b))
    {
      PairPrior kept = pair;

      // A closer this class does not have cannot close anything.
      kept.closedBy.clear();

      for (const RoomUse closer : pair.closedBy)
      {
        if (inVocabulary(closer))
        {
          kept.closedBy.push_back(closer);
        }
      }

      p.pairs.push_back(std::move(kept));
    }
  }

  for (const PairPrior &pair : profile.extraPairs)
  {
    if (inVocabulary(pair.a) && inVocabulary(pair.b))
    {
      p.pairs.push_back(pair);
    }
  }

  p.exists.fill(BayesianNetwork::invalid);
  p.area.fill(BayesianNetwork::invalid);
  p.aspect.fill(BayesianNetwork::invalid);

  // --- the global variables ---

  p.standing = net.addVariable(
    "Standing", {"Slum", "Modest", "Comfortable", "Wealthy", "Grand", "Rural"});
  net.setPrior(p.standing, {1, 1, 1, 1, 1, 1});  // Always clamped by the brief.

  // Four, not three.
  //
  // The corpus has four-storey buildings in it - a first class town mansion of
  // forty-three rooms, a town house, a detached town house - and a variable
  // that stops at three cannot represent them: the brief that asks for one
  // comes back "the network gives it probability zero", which is true of the
  // network and false of the book. A town house on a narrow plot goes up
  // because it cannot go outward, and so does a mansion with a servants'
  // floor in the roof.
  p.storeys = net.addVariable("Storeys", {"1", "2", "3", "4"});
  net.setParents(p.storeys, {p.standing});
  net.setDistribution(p.storeys, {
    /* Slum        */ {.35, .59, .05, .01},
    /* Modest      */ {.29, .64, .06, .01},
    /* Comfortable */ {.10, .68, .20, .02},
    /* Wealthy     */ {.02, .53, .40, .05},
    /* Grand       */ {.01, .30, .58, .11},
    /* Rural       */ {.55, .44, .01, .00},
  });

  // A basement is a storey of these houses rather than a hole under them:
  // corpus page 27 is a *basement plan* carrying the kitchen, scullery,
  // larder and dairy, with the elevation showing two storeys over it.
  p.basement = net.addBoolean("BasementExists");
  net.setParents(p.basement, {p.standing, p.storeys});
  {
    const std::array<double, standingCount> base = {.02, .08, .30, .70, .88, .06};
    std::vector<std::vector<double>> rows;

    for (uint32_t s = 0; s < standingCount; ++s)
    {
      // A taller house is a deeper one: the same builder digs out when he
      // builds up.
      for (const double lift : {.45, 1., 1.35, 1.5})
      {
        rows.push_back(yesNo(base[s] * lift));
      }
    }

    net.setDistribution(p.basement, rows);
  }

  p.totalArea = net.addVariable("TotalArea", binStates(kTotalEdges));
  net.setParents(p.totalArea, {p.standing, p.storeys});
  {
    const std::array<double, standingCount> typical = {420, 750, 1250, 2300, 4200, 900};
    std::vector<std::vector<double>> rows;

    for (uint32_t s = 0; s < standingCount; ++s)
    {
      for (const double byStoreys : {.72, 1., 1.30, 1.58})
      {
        rows.push_back(lognormalOverBins(kTotalEdges,
                                         typical[s] * byStoreys * profile.sizeScale, .30));
      }
    }

    net.setDistribution(p.totalArea, rows);
  }

  // The footprint's proportions. Its *area* is the total over the storeys, so
  // this node and TotalArea together are the paper's Footprint variable.
  p.footprintAspect = net.addVariable("FootprintAspect", {"1:1", "5:4", "3:2", "2:1", ">2:1"});
  net.setParents(p.footprintAspect, {p.totalArea});
  net.setDistribution(p.footprintAspect, {
    // A one-room-deep cottage is a long thin thing; there is no other way to
    // get light into it.
    /* 150-300  */ {.07, .22, .34, .26, .11},
    /* 300-450  */ {.08, .25, .34, .24, .09},
    /* 450-600  */ {.10, .28, .34, .21, .07},
    /* 600-900  */ {.10, .30, .34, .19, .07},
    /* 900-1300 */ {.11, .31, .33, .18, .07},
    /* 1300-1900*/ {.12, .32, .32, .17, .07},
    // A big house is not a big box: it acquires wings, and the bounding
    // rectangle of a house with wings is longer than the house it grew from.
    /* 1900-2800*/ {.10, .28, .32, .21, .09},
    /* 2800-4200*/ {.08, .24, .32, .24, .12},
    /* >4200    */ {.06, .20, .31, .27, .16},
  });

  // --- how many bedrooms, and how many closets ---

  // Ten, not six.
  //
  // The corpus's first class town mansion has seven bed rooms on its first
  // floor alone and more over them; a variable that stops at six cannot say
  // so, and the house it draws instead has a great ground floor and two thin
  // storeys of nothing over it - which is what the plans looked like.
  //
  // The rows are generated rather than written out, because sixty of them by
  // hand is sixty chances to fat-finger a row that still sums to one. The
  // shape is a discretised lognormal about a mean that rises with the standing
  // and with the number of floors there are to put bedrooms on.
  p.bedroomCount = net.addVariable("BedroomCount", numberedStates(0, 10));
  net.setParents(p.bedroomCount, {p.standing, p.storeys});
  {
    // Mean bedrooms by standing, for one, two, three and four storeys.
    const std::array<std::array<double, 4>, standingCount> means = {{
      /* Slum        */ {{1.4, 2.1, 2.8, 3.3}},
      /* Modest      */ {{1.7, 2.8, 3.7, 4.3}},
      /* Comfortable */ {{2.2, 3.5, 4.6, 5.4}},
      /* Wealthy     */ {{2.8, 4.4, 5.9, 7.1}},
      /* Grand       */ {{3.4, 5.4, 7.3, 8.8}},
      /* Rural       */ {{1.6, 2.6, 3.5, 4.1}},
    }};

    std::vector<std::vector<double>> rows;

    for (uint32_t s = 0; s < standingCount; ++s)
    {
      for (uint32_t storeys = 0; storeys < 4; ++storeys)
      {
        const double mean = means[s][storeys];
        const double sigma = .42;
        std::vector<double> row(11, 0.);
        double mass = 0;

        for (uint32_t n = 0; n < row.size(); ++n)
        {
          // Zero bedrooms is a real outcome only in the smallest houses - a
          // one-room cottage sleeps in the room it lives in - so it is given
          // the tail below one rather than a bin of its own.
          const double at = std::max(double(n), .55);
          const double t = (std::log(at) - std::log(mean)) / sigma;
          const double weight = std::exp(-.5 * t * t) / at + 1e-4;

          row[n] = weight;
          mass += weight;
        }

        for (double &weight : row)
        {
          weight /= mass;
        }

        rows.push_back(std::move(row));
      }
    }

    net.setDistribution(p.bedroomCount, rows);
  }

  p.closetCount = net.addVariable("ClosetCount", numberedStates(0, 4));
  net.setParents(p.closetCount, {p.standing, p.bedroomCount});
  {
    // A closet belongs to a bedroom in these plans - it is a cupboard cut out
    // of the chamber, drawn and labelled - so the count follows the bedrooms
    // and the standing decides how many of them get one.
    const std::array<double, standingCount> perBedroom = {.15, .35, .60, .85, .95, .30};
    std::vector<std::vector<double>> rows;

    for (uint32_t s = 0; s < standingCount; ++s)
    {
      for (uint32_t beds = 0; beds <= 10; ++beds)
      {
        const double mean = std::min(double(beds) * perBedroom[s], 4.);
        std::vector<double> row(5, 0.);

        // Poisson-ish about `mean`, truncated at four.
        double sum = 0.;

        for (uint32_t k = 0; k < 5; ++k)
        {
          const double d = double(k) - mean;
          row[k] = std::exp(-.9 * d * d) + .01;
          sum += row[k];
        }

        for (double &v : row)
        {
          v /= sum;
        }

        rows.push_back(row);
      }
    }

    net.setDistribution(p.closetCount, rows);
  }

  // --- one existence node per room ---

  const std::array<RoomPrior, roomUseCount> &priors = roomPriors();

  // A class's own existence probabilities, where it has any, standing in for
  // the shared table's.
  const auto existsFor = [&profile, &priors](const RoomUse use) {
    for (const auto &entry : profile.existsOverride)
    {
      if (entry.first == use)
      {
        return entry.second;
      }
    }

    return priors[uint32_t(use)].exists;
  };

  for (const RoomUse use : profile.rooms)
  {
    const uint32_t u = uint32_t(use);

    // Bedrooms and closets are counted, not switched; their existence node is
    // a deterministic read of the count so that the adjacency nodes below can
    // all hang off booleans and be authored the same way.
    if (use == RoomUse::Bedroom || use == RoomUse::Closet)
    {
      const uint32_t count = use == RoomUse::Bedroom ? p.bedroomCount : p.closetCount;
      const uint32_t node = net.addBoolean(existsNodeName(use));
      net.setParents(node, {count});

      std::vector<std::vector<double>> rows;

      for (uint32_t k = 0; k < net.variableAt(count).cardinality(); ++k)
      {
        rows.push_back(k == 0 ? std::vector<double>{1., 0.} : std::vector<double>{0., 1.});
      }

      net.setDistribution(node, rows);
      p.exists[u] = node;

      continue;
    }

    const uint32_t node = net.addBoolean(existsNodeName(use));

    // The rooms whose existence depends on more than the standing. These are
    // the conditional dependencies the paper's argument is about: a larder is
    // a room off a scullery, a breakfast room belongs to a house that already
    // has a drawing room, and a landing is what a staircase arrives at.
    // Several rooms hang off another room's existence. When this class has no
    // such room the dependence cannot be wired, and the node falls back to the
    // plain form on the standing alone - which is why each of these asks.
    const auto present = [&p](const RoomUse other) {
      return p.exists[uint32_t(other)] != BayesianNetwork::invalid;
    };

    if (use == RoomUse::Scullery && present(RoomUse::Kitchen))
    {
      // A house with no kitchen usually has a scullery instead - but not
      // always, and the certainty is one link further along the chain.
      net.setParents(node, {p.standing, p.exists[uint32_t(RoomUse::Kitchen)]});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        rows.push_back(yesNo(.75));                 // no kitchen: a scullery, usually
        rows.push_back(yesNo(existsFor(use)[s])); // a kitchen: as the standing has it
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::LivingRoom && profile.dwelling
             && present(RoomUse::Kitchen) && present(RoomUse::Scullery))
    {
      // A dwelling has somewhere to cook, and this is where that certainty
      // finally lands. The hand-authored network said it by making the kitchen
      // certain; the corpus says that is the wrong room twice over. Its
      // cottages have a living room with the fire in it and a scullery behind
      // and no room labelled kitchen - and its smallest, the five-roomed
      // labourer's cottage of plate 32, has a living room and a washhouse and
      // not even a scullery. So: a house with neither a kitchen nor a scullery
      // has a living room, certainly, and cooks in it.
      //
      // Only a network can state that, and it survives fitting because it is a
      // hard one in the table rather than a high probability.
      net.setParents(node, {p.standing, p.exists[uint32_t(RoomUse::Kitchen)],
                            p.exists[uint32_t(RoomUse::Scullery)]});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        rows.push_back(yesNo(1.));                       // no kitchen, no scullery
        rows.push_back(yesNo(existsFor(use)[s]));      // no kitchen, a scullery
        rows.push_back(yesNo(existsFor(use)[s]));      // a kitchen, no scullery
        rows.push_back(yesNo(existsFor(use)[s] * .7)); // a kitchen and a scullery
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::Larder && present(RoomUse::Scullery))
    {
      net.setParents(node, {p.standing, p.exists[uint32_t(RoomUse::Scullery)]});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        rows.push_back(yesNo(existsFor(use)[s] * .30));  // no scullery
        rows.push_back(yesNo(existsFor(use)[s]));        // a scullery
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::BreakfastRoom && present(RoomUse::DiningRoom))
    {
      net.setParents(node, {p.standing, p.exists[uint32_t(RoomUse::DiningRoom)]});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        rows.push_back(yesNo(existsFor(use)[s] * .12));
        rows.push_back(yesNo(existsFor(use)[s]));
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::Passage && present(RoomUse::Hall))
    {
      net.setParents(node, {p.standing, p.exists[uint32_t(RoomUse::Hall)], p.totalArea});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        for (const double hall : {.55, 1.15})
        {
          for (uint32_t a = 0; a + 1 < kTotalEdges.size(); ++a)
          {
            // A passage is what a plan needs once it is too deep for one
            // room to open off another. Small houses have none.
            const double bySize = .30 + .11 * double(a);
            rows.push_back(yesNo(existsFor(use)[s] * hall * bySize));
          }
        }
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::Landing)
    {
      net.setParents(node, {p.standing, p.storeys});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        // There is nothing to land on in a single-storey house, and a stair
        // in a taller one arrives somewhere - in a cottage that somewhere is
        // a bedroom, which is why this is high rather than certain.
        for (const double byStoreys : {.03, 2.20, 2.60, 2.80})
        {
          rows.push_back(yesNo(existsFor(use)[s] * byStoreys));
        }
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::Staircase)
    {
      net.setParents(node, {p.standing, p.storeys, p.basement});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        for (uint32_t storeys = 1; storeys <= 4; ++storeys)
        {
          for (const bool basement : {false, true})
          {
            // A floor above or below has to be reached, and the only thing
            // that reaches it is a stair. Below that, a stair is a thing a
            // house might have for an attic, and mostly does not.
            const bool needed = storeys > 1 || basement;

            rows.push_back(yesNo(needed ? 1. : existsFor(use)[s] * .30));
          }
        }
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::Cellar)
    {
      net.setParents(node, {p.standing, p.basement});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        rows.push_back(yesNo(existsFor(use)[s] * .35));
        rows.push_back(yesNo(existsFor(use)[s] * 1.10));
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::Hall)
    {
      net.setParents(node, {p.standing, p.storeys});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        for (const double byStoreys : {.80, 1.05, 1.10, 1.12})
        {
          rows.push_back(yesNo(existsFor(use)[s] * byStoreys));
        }
      }

      net.setDistribution(node, rows);
    }
    else if (use == RoomUse::DressingRoom || use == RoomUse::Nursery)
    {
      net.setParents(node, {p.standing, p.bedroomCount});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        for (uint32_t beds = 0; beds <= 10; ++beds)
        {
          // Both are rooms a house acquires once it has bedrooms to spare.
          const double byBeds = beds == 0 ? 0. : std::min(.25 * double(beds), 1.1);
          rows.push_back(yesNo(existsFor(use)[s] * byBeds));
        }
      }

      net.setDistribution(node, rows);
    }
    else
    {
      net.setParents(node, {p.standing});

      std::vector<std::vector<double>> rows;

      for (uint32_t s = 0; s < standingCount; ++s)
      {
        rows.push_back(yesNo(existsFor(use)[s]));
      }

      net.setDistribution(node, rows);
    }

    p.exists[u] = node;
  }

  // --- area, and the aspect ratio hanging off it ---

  // The paper leaves aspect ratios out of structure learning entirely and
  // wires each to its own Area node in an automatic post-process, because the
  // dependence is known a priori and searching for it halves the variable
  // count for nothing. Authored by hand the saving is the same: this loop is
  // the post-process.
  for (const RoomUse use : profile.rooms)
  {
    const uint32_t u = uint32_t(use);
    const RoomPrior &prior = priors[u];

    const uint32_t areaNode = net.addVariable(areaNodeName(use), binStates(kAreaEdges));
    net.setParents(areaNode, {p.totalArea});

    std::vector<std::vector<double>> areaRows;

    for (uint32_t a = 0; a + 1 < kTotalEdges.size(); ++a)
    {
      const double houseSqFt = representative(kTotalEdges, a);
      const double scale = std::pow(houseSqFt / referenceTotalSqFt, prior.growth);

      areaRows.push_back(lognormalOverBins(kAreaEdges, prior.typicalSqFt * scale, prior.sigma));
    }

    net.setDistribution(areaNode, areaRows);
    p.area[u] = areaNode;

    const uint32_t aspectNode = net.addVariable(aspectNodeName(use), {"1:1", "5:4", "3:2", "2:1", ">2:1"});
    net.setParents(aspectNode, {areaNode});

    std::vector<std::vector<double>> aspectRows;

    for (uint32_t bin = 0; bin + 1 < kAreaEdges.size(); ++bin)
    {
      // A big room is squarer than a small one of the same use: the long
      // dimension of a room is set by what it is for, and the short one by how
      // much floor it was given.
      const double sizeFraction = double(bin) / double(kAreaEdges.size() - 2);
      const double stretch = prior.elongation * (1.25 - .5 * sizeFraction);

      std::vector<double> row(kAspects.size(), 0.);
      double sum = 0.;

      for (size_t i = 0; i < kAspects.size(); ++i)
      {
        const double position = double(i) / double(kAspects.size() - 1);
        const double d = position - stretch;

        row[i] = std::exp(-8. * d * d) + .01;
        sum += row[i];
      }

      for (double &v : row)
      {
        v /= sum;
      }

      aspectRows.push_back(row);
    }

    net.setDistribution(aspectNode, aspectRows);
    p.aspect[u] = aspectNode;
  }

  // --- a pair of nodes per room pair ---

  for (const PairPrior &pair : p.pairs)
  {
    const uint32_t adjacency = net.addBoolean(adjacencyNodeName(pair.a, pair.b));

    std::vector<uint32_t> parents = {p.exists[uint32_t(pair.a)], p.exists[uint32_t(pair.b)]};

    for (const RoomUse closer : pair.closedBy)
    {
      parents.push_back(p.exists[uint32_t(closer)]);
    }

    net.setParents(adjacency, parents);

    // Two rooms that are not both there cannot open into each other. That is
    // the hard zero in this network and it belongs here rather than in the
    // decoder, so that clamping an adjacency on propagates back to the rooms
    // it needs.
    //
    // The rows run over the parent assignments with the last parent varying
    // fastest, which is the two existence nodes and then whatever closes the
    // pair, so they are generated rather than written out.
    std::vector<std::vector<double>> rows;
    const uint32_t closers = uint32_t(pair.closedBy.size());

    for (uint32_t assignment = 0; assignment < (1u << (2 + closers)); ++assignment)
    {
      const uint32_t bits = uint32_t(1) << (2 + closers);
      const bool hasA = (assignment & (bits >> 1)) != 0;
      const bool hasB = (assignment & (bits >> 2)) != 0;

      bool closed = false;

      for (uint32_t c = 0; c < closers; ++c)
      {
        closed = closed || (assignment & (bits >> (3 + c))) != 0;
      }

      rows.push_back(hasA && hasB ? yesNo(closed ? pair.whenClosed : pair.adjacent)
                                  : std::vector<double>{1., 0.});
    }

    net.setDistribution(adjacency, rows);

    const uint32_t link = net.addVariable(adjacencyTypeNodeName(pair.a, pair.b), {"open", "door"});
    net.setParents(link, {adjacency});
    net.setDistribution(link, {
      /* not adjacent */ {.5, .5},  // Never read; the decoder skips it.
      /* adjacent     */ {1. - clampProbability(pair.door), clampProbability(pair.door)},
    });

    p.pairNodes.push_back({adjacency, link});
  }

  net.validate(p.invalidWhy);

  return p;
}

const Prior &prior(const BuildingClass klass)
{
  static const std::vector<Prior> built = [] {
    std::vector<Prior> all;

    for (uint32_t k = 0; k < uint32_t(BuildingClass::Count); ++k)
    {
      all.push_back(buildPrior(BuildingClass(k)));
    }

    return all;
  }();

  return built[size_t(klass) < built.size() ? size_t(klass) : 0];
}

// The fitted network for a class, when one has been installed. It has the same
// variables, parents and shapes as that class's prior - only the numbers
// differ - so every node index in `Prior` still addresses the right variable.
BayesianNetwork &fittedNetwork(const BuildingClass klass)
{
  static std::vector<BayesianNetwork> fitted =
    std::vector<BayesianNetwork>(size_t(BuildingClass::Count));

  return fitted[size_t(klass) < fitted.size() ? size_t(klass) : 0];
}

// Which network is in force. `Unset` means nobody has said, and the asset is
// consulted once to decide; a caller that installs one, or asks for the prior,
// settles it and the asset is never looked at again. Without that distinction
// the lazy asset load runs on the first *use* and silently overrides a caller
// who asked for the prior before using anything - which is exactly what a test
// of the hand-authored network does.
enum class Selection : uint8_t
{
  Unset,
  Prior,
  Fitted,
};

Selection &selection(const BuildingClass klass)
{
  static std::vector<Selection> chosen =
    std::vector<Selection>(size_t(BuildingClass::Count), Selection::Unset);

  return chosen[size_t(klass) < chosen.size() ? size_t(klass) : 0];
}

// --- sampling helpers ----------------------------------------------------

struct Rng
{
  uint64_t state;

  explicit Rng(const uint64_t seed) : state(seed ^ 0xD1B54A32D192ED03ull) {}

  uint64_t next()
  {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;

    return z ^ (z >> 31);
  }

  double unit() { return double(next() >> 11) * 0x1.0p-53; }
};

uint32_t pick(const std::vector<double> &weights, Rng &rng)
{
  double sum = 0.;

  for (const double w : weights)
  {
    sum += w;
  }

  const double u = rng.unit() * sum;
  double running = 0.;

  for (uint32_t i = 0; i < weights.size(); ++i)
  {
    running += weights[i];

    if (u < running)
    {
      return i;
    }
  }

  return weights.empty() ? 0 : uint32_t(weights.size()) - 1;
}

// Two rooms can only open into each other if they are on the same floor. The
// exception is the stair, which is the one room that belongs to more than one
// of them - it is how the basement kitchen is reached from the hall and how
// the landing is reached at all.
//
// Without this the network cheerfully puts a door between an upstairs bedroom
// and the ground-floor passage, because the adjacency nodes know which rooms
// exist and nothing about where they are. The floors are decided in the
// decoder, so the rule is applied here.
bool floorsMeet(const RoomSpec &a, const RoomSpec &b)
{
  return a.floor == b.floor || a.use == RoomUse::Staircase || b.use == RoomUse::Staircase;
}

// Which instance of a room a link should use, given the floor it has to reach.
//
// A three-storey house has a landing on each upper floor, not one landing for
// both, and its fourth bedroom is on the floor its second landing serves. So
// pairing the k-th bedroom with the k-th landing is wrong: the pairing has to
// follow the floors. Search from the k-th instance and take the first one on
// the right floor, so distinct instances still get distinct partners where
// there are several to go round.
uint32_t instanceReaching(const Programme &programme, const RoomUse use,
                          const uint32_t count, const uint32_t k, const int32_t floor)
{
  const uint32_t start = std::min(k, count - 1);

  for (uint32_t step = 0; step < count; ++step)
  {
    const uint32_t room = programme.find(use, (start + step) % count);

    if (room != Programme::npos
        && (programme.rooms[room].floor == floor || use == RoomUse::Staircase))
    {
      return room;
    }
  }

  return programme.find(use, start);
}

bool isCirculation(const RoomUse use)
{
  return use == RoomUse::Hall || use == RoomUse::Passage || use == RoomUse::Landing
         || use == RoomUse::Staircase || use == RoomUse::Lobby || use == RoomUse::Porch;
}

// Every room has to be reachable from the front door, and the network cannot
// promise that: each adjacency is drawn on its own given the rooms, so a house
// of thirty rooms will now and then draw a dairy whose only partner - the
// scullery - did not open into it. A room with no way in is not a room.
//
// So the programme is closed here, and the rule is the plan's structural one:
// circulation is a room, and it is what a stranded room joins. A stranded room
// takes, in order, a circulation room on its own floor, then a room the pair
// table says it belongs with, then any circulation at all - which is how the
// basement gets found, because the only thing that reaches it is the stair.
// The last resort is the entrance itself.
//
// Nothing outside the pair table and the circulation set is ever joined, so
// the repair cannot invent the kitchen-bathroom-bedroom path the network
// exists to prevent.
void connectProgramme(Programme &programme, const std::vector<PairPrior> &pairs)
{
  if (programme.rooms.empty())
  {
    return;
  }

  uint32_t entrance = programme.find(RoomUse::Porch);

  if (entrance == Programme::npos)
  {
    entrance = programme.find(RoomUse::Hall);
  }

  if (entrance == Programme::npos)
  {
    entrance = programme.find(RoomUse::Kitchen);
  }

  if (entrance == Programme::npos)
  {
    entrance = 0;
  }

  const uint32_t rooms = uint32_t(programme.rooms.size());

  // Who the pair table says each room belongs with.
  std::vector<std::vector<uint32_t>> partners(rooms);

  for (const PairPrior &pair : pairs)
  {
    for (uint32_t a = 0; a < rooms; ++a)
    {
      if (programme.rooms[a].use != pair.a)
      {
        continue;
      }

      for (uint32_t b = 0; b < rooms; ++b)
      {
        if (programme.rooms[b].use == pair.b)
        {
          partners[a].push_back(b);
          partners[b].push_back(a);
        }
      }
    }
  }

  for (;;)
  {
    std::vector<std::vector<uint32_t>> neighbours(rooms);

    for (const Adjacency &link : programme.links)
    {
      neighbours[link.a].push_back(link.b);
      neighbours[link.b].push_back(link.a);
    }

    std::vector<bool> reached(rooms, false);
    std::vector<uint32_t> stack = {entrance};
    reached[entrance] = true;

    while (!stack.empty())
    {
      const uint32_t r = stack.back();
      stack.pop_back();

      for (const uint32_t n : neighbours[r])
      {
        if (!reached[n])
        {
          reached[n] = true;
          stack.push_back(n);
        }
      }
    }

    uint32_t stranded = rooms;

    for (uint32_t r = 0; r < rooms; ++r)
    {
      if (!reached[r])
      {
        stranded = r;

        break;
      }
    }

    if (stranded == rooms)
    {
      return;
    }

    uint32_t host = Programme::npos;

    for (uint32_t r = 0; r < rooms && host == Programme::npos; ++r)
    {
      if (reached[r] && isCirculation(programme.rooms[r].use)
          && programme.rooms[r].floor == programme.rooms[stranded].floor
          && programme.rooms[r].use != RoomUse::Staircase)
      {
        host = r;
      }
    }

    for (const uint32_t partner : partners[stranded])
    {
      if (host == Programme::npos && reached[partner]
          && floorsMeet(programme.rooms[partner], programme.rooms[stranded]))
      {
        host = partner;
      }
    }

    // The stair, which reaches every floor. This is what finds the basement.
    for (uint32_t r = 0; r < rooms && host == Programme::npos; ++r)
    {
      if (reached[r] && programme.rooms[r].use == RoomUse::Staircase)
      {
        host = r;
      }
    }

    for (uint32_t r = 0; r < rooms && host == Programme::npos; ++r)
    {
      if (reached[r] && isCirculation(programme.rooms[r].use)
          && floorsMeet(programme.rooms[r], programme.rooms[stranded]))
      {
        host = r;
      }
    }

    for (uint32_t r = 0; r < rooms && host == Programme::npos; ++r)
    {
      if (reached[r] && floorsMeet(programme.rooms[r], programme.rooms[stranded]))
      {
        host = r;
      }
    }

    if (host == Programme::npos)
    {
      host = entrance;
    }

    programme.links.push_back({host, stranded, true});
    ++programme.repairedLinks;
  }
}

int32_t floorFor(const Band band, const uint32_t storeys, const bool basement,
                 const uint32_t instance)
{
  switch (band)
  {
    case Band::Cellar:
      return basement ? -1 : 0;

    case Band::Service:
      return basement ? -1 : 0;

    case Band::Upper:
      // Bedrooms fill the upper floors from the bottom up, which is what
      // stacking a plan actually does.
      return storeys <= 1 ? 0 : int32_t(1 + instance % (storeys - 1));

    case Band::Ground:
    default:
      return 0;
  }
}

}  // namespace

// --- names ---------------------------------------------------------------

bool enclosesFloorArea(const RoomUse use)
{
  // The yard is ground, not floor. The corpus draws it and labels it, and the
  // plan's adjacency graph needs it because the back door opens onto it - but
  // it is outside, and anything summing the house's area has to leave it out
  // or a cottage comes out twice the size it is.
  return use != RoomUse::Yard && use != RoomUse::Courtyard
         && use != RoomUse::ExerciseYard && use != RoomUse::Croft
         && use != RoomUse::Garden && use != RoomUse::Orchard && use != RoomUse::Pen
         && use != RoomUse::Midden;
}

const char *name(const PlotStanding standing)
{
  switch (standing)
  {
    case PlotStanding::Slum: return "poor";
    case PlotStanding::Rural: return "rural";
    case PlotStanding::Modest: return "modest";
    case PlotStanding::Comfortable: return "comfortable";
    case PlotStanding::Wealthy: return "wealthy";
    case PlotStanding::Grand: return "grand";
  }

  return "?";
}

const char *name(const BuildingClass klass)
{
  switch (klass)
  {
    case BuildingClass::Dwelling: return "dwelling";
    case BuildingClass::ShopHouse: return "shop house";
    case BuildingClass::Inn: return "inn";
    case BuildingClass::School: return "school";
    case BuildingClass::Dispensary: return "dispensary";
    case BuildingClass::Warehouse: return "warehouse";
    case BuildingClass::Farmstead: return "farmstead";
    case BuildingClass::Church: return "church";
    case BuildingClass::Chapel: return "chapel";
    case BuildingClass::Temple: return "temple";
    case BuildingClass::Castle: return "castle";
    case BuildingClass::ManorHouse: return "manor house";
    case BuildingClass::Prison: return "prison";
    case BuildingClass::Mansion: return "mansion";
    case BuildingClass::GuildHall: return "guild hall";
    case BuildingClass::PeasantCottage: return "peasant cottage";
    case BuildingClass::Dormitory: return "lodging house";
    case BuildingClass::CommerceHouse: return "commerce house";
    case BuildingClass::Count: break;
  }

  return "?";
}

const char *name(const RoomUse use)
{
  switch (use)
  {
    case RoomUse::Kitchen: return "kitchen";
    case RoomUse::Scullery: return "scullery";
    case RoomUse::Larder: return "larder";
    case RoomUse::Pantry: return "pantry";
    case RoomUse::Dairy: return "dairy";
    case RoomUse::Washhouse: return "washhouse";
    case RoomUse::Cellar: return "cellar";
    case RoomUse::Parlour: return "parlour";
    case RoomUse::LivingRoom: return "living room";
    case RoomUse::DiningRoom: return "dining room";
    case RoomUse::DrawingRoom: return "drawing room";
    case RoomUse::BreakfastRoom: return "breakfast room";
    case RoomUse::Study: return "study";
    case RoomUse::Library: return "library";
    case RoomUse::Hall: return "hall";
    case RoomUse::Passage: return "passage";
    case RoomUse::Landing: return "landing";
    case RoomUse::Staircase: return "staircase";
    case RoomUse::Lobby: return "lobby";
    case RoomUse::Porch: return "porch";
    case RoomUse::Bedroom: return "bedroom";
    case RoomUse::DressingRoom: return "dressing room";
    case RoomUse::Nursery: return "nursery";
    case RoomUse::Closet: return "closet";
    case RoomUse::WaterCloset: return "water closet";
    case RoomUse::Bathroom: return "bathroom";
    case RoomUse::Office: return "office";
    case RoomUse::Shop: return "shop";
    case RoomUse::Workshop: return "workshop";
    case RoomUse::Store: return "store";
    case RoomUse::Yard: return "yard";
    case RoomUse::MorningRoom: return "morning room";
    case RoomUse::Boudoir: return "boudoir";
    case RoomUse::Conservatory: return "conservatory";
    case RoomUse::Veranda: return "veranda";
    case RoomUse::ServantsHall: return "servants hall";
    case RoomUse::Brewhouse: return "brewhouse";
    case RoomUse::Bar: return "bar";
    case RoomUse::TapRoom: return "tap room";
    case RoomUse::ClubRoom: return "club room";
    case RoomUse::CoffeeRoom: return "coffee room";
    case RoomUse::BilliardRoom: return "billiard room";
    case RoomUse::SchoolRoom: return "school room";
    case RoomUse::Dispensary: return "dispensary";
    case RoomUse::WaitingRoom: return "waiting room";
    case RoomUse::ConsultingRoom: return "consulting room";
    case RoomUse::Stable: return "stable";
    case RoomUse::Barn: return "barn";
    case RoomUse::Loft: return "loft";
    case RoomUse::Nave: return "nave";
    case RoomUse::Chancel: return "chancel";
    case RoomUse::Aisle: return "aisle";
    case RoomUse::Transept: return "transept";
    case RoomUse::Sanctuary: return "sanctuary";
    case RoomUse::Narthex: return "narthex";
    case RoomUse::SideChapel: return "side chapel";
    case RoomUse::Vestry: return "vestry";
    case RoomUse::Crypt: return "crypt";
    case RoomUse::Belfry: return "belfry";
    case RoomUse::Cloister: return "cloister";
    case RoomUse::ChapterHouse: return "chapter house";
    case RoomUse::Gallery: return "gallery";
    case RoomUse::GreatHall: return "great hall";
    case RoomUse::ScreensPassage: return "screens passage";
    case RoomUse::Solar: return "solar";
    case RoomUse::Buttery: return "buttery";
    case RoomUse::Keep: return "keep";
    case RoomUse::Gatehouse: return "gatehouse";
    case RoomUse::Guardroom: return "guardroom";
    case RoomUse::Barracks: return "barracks";
    case RoomUse::Armoury: return "armoury";
    case RoomUse::Dungeon: return "dungeon";
    case RoomUse::Tower: return "tower";
    case RoomUse::Courtyard: return "courtyard";
    case RoomUse::Well: return "well";
    case RoomUse::Cell: return "cell";
    case RoomUse::Infirmary: return "infirmary";
    case RoomUse::ExerciseYard: return "exercise yard";
    case RoomUse::Saloon: return "saloon";
    case RoomUse::Ballroom: return "ballroom";
    case RoomUse::SmokingRoom: return "smoking room";
    case RoomUse::GunRoom: return "gun room";
    case RoomUse::StillRoom: return "still room";
    case RoomUse::HousekeepersRoom: return "housekeeper's room";
    case RoomUse::ButlersRoom: return "butler's room";
    case RoomUse::CountingHouse: return "counting house";
    case RoomUse::StrongRoom: return "strong room";
    case RoomUse::CouncilChamber: return "council chamber";
    case RoomUse::Undercroft: return "undercroft";
    case RoomUse::Byre: return "byre";
    case RoomUse::Croft: return "croft";
    case RoomUse::Garden: return "garden";
    case RoomUse::Orchard: return "orchard";
    case RoomUse::Pen: return "pen";
    case RoomUse::Midden: return "midden";
    case RoomUse::Dormitory: return "dormitory";
    case RoomUse::Refectory: return "refectory";
    case RoomUse::Count: break;
  }

  return "?";
}

namespace {

// "dining room" -> "DiningRoom", so a node name reads as a node name.
std::string camel(const RoomUse use)
{
  std::string out;
  bool upper = true;

  for (const char *c = name(use); *c; ++c)
  {
    if (*c == ' ')
    {
      upper = true;

      continue;
    }

    out.push_back(upper ? char(std::toupper(*c)) : *c);
    upper = false;
  }

  return out;
}

}  // namespace

std::string existsNodeName(const RoomUse use) { return camel(use) + "Exists"; }
std::string areaNodeName(const RoomUse use) { return camel(use) + "Area"; }
std::string aspectNodeName(const RoomUse use) { return camel(use) + "AspectRatio"; }

std::string adjacencyNodeName(const RoomUse a, const RoomUse b)
{
  return camel(a) + "-" + camel(b) + "-Adjacency";
}

std::string adjacencyTypeNodeName(const RoomUse a, const RoomUse b)
{
  return camel(a) + "-" + camel(b) + "-AdjacencyType";
}

const std::vector<double> &programmeAreaBinEdgesSqFt() { return kAreaEdges; }
const std::vector<double> &programmeAspectStates() { return kAspects; }

const std::vector<std::pair<RoomUse, RoomUse>> &programmeAdjacencyPairs()
{
  static const std::vector<std::pair<RoomUse, RoomUse>> pairs = [] {
    std::vector<std::pair<RoomUse, RoomUse>> out;

    for (const PairPrior &p : pairPriors())
    {
      out.emplace_back(p.a, p.b);
    }

    return out;
  }();

  return pairs;
}

const BayesianNetwork &priorProgrammeNetwork(const BuildingClass klass)
{
  return prior(klass).net;
}

const std::vector<RoomUse> &programmeVocabulary(const BuildingClass klass)
{
  return prior(klass).vocabulary;
}

void installProgrammeNetwork(const BayesianNetwork *network, const BuildingClass klass)
{
  // Settling the choice races with reading it; see activeProgrammeNetwork.
  static std::mutex installing;
  std::lock_guard<std::mutex> held(installing);

  if (network == nullptr)
  {
    selection(klass) = Selection::Prior;

    return;
  }

  fittedNetwork(klass) = *network;
  selection(klass) = Selection::Fitted;
}

const BayesianNetwork &activeProgrammeNetwork(const BuildingClass klass)
{
  // One lock over the decision, because houses are planned in parallel.
  //
  // A region's buildings are generated on whatever worker picks the region up,
  // and a town planned across threads calls this from all of them at once. The
  // statics behind it initialise safely - that much C++ guarantees - but the
  // *decision* written into them here does not: two threads arriving with the
  // class Unset would both read the asset and both assign, one of them into a
  // network the other is already sampling from.
  //
  // The lock is taken once per class and never again: after the first call the
  // selection is settled, so the fast path is a read of a value that no longer
  // changes.
  static std::mutex deciding;
  std::lock_guard<std::mutex> once(deciding);

  // The trained network is an asset, consulted once when nobody has said
  // otherwise. If it is not there the hand-authored prior stands, which is the
  // honest fallback: the prior is a working generator and a missing file is
  // not a reason to stop making houses.
  if (selection(klass) == Selection::Unset)
  {
    selection(klass) = Selection::Prior;

#ifdef ARCHITECTURAL_PROGRAMME_NETWORK_DIR
    // One asset per class, named by the class. A class with no trained file
    // keeps its hand-authored prior, which is most of them: only the dwelling
    // has enough transcribed examples to fit.
    const std::string path = std::string(ARCHITECTURAL_PROGRAMME_NETWORK_DIR)
                             + "/programme-network-" + name(klass) + ".json";
    std::ifstream stream(path, std::ios::binary);

    if (stream)
    {
      const std::string text((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());

      BayesianNetwork network;
      std::string why;

      if (network.fromJson(text, why))
      {
        fittedNetwork(klass) = std::move(network);
        selection(klass) = Selection::Fitted;
      }
    }
#endif
  }

  return selection(klass) == Selection::Fitted ? fittedNetwork(klass) : prior(klass).net;
}

const std::vector<double> &programmeTotalAreaBinEdgesSqFt() { return kTotalEdges; }

uint32_t programmeTotalAreaBin(const double squareFeet)
{
  return binOf(kTotalEdges, squareFeet);
}

uint32_t programmeAreaBin(const double squareFeet) { return binOf(kAreaEdges, squareFeet); }

uint32_t programmeAspectState(const double longOverShort)
{
  uint32_t best = 0;

  for (uint32_t i = 1; i < kAspects.size(); ++i)
  {
    if (std::abs(kAspects[i] - longOverShort) < std::abs(kAspects[best] - longOverShort))
    {
      best = i;
    }
  }

  return best;
}

// --- the programme -------------------------------------------------------

uint32_t Programme::countOf(const RoomUse use) const
{
  uint32_t n = 0;

  for (const RoomSpec &r : rooms)
  {
    n += r.use == use ? 1 : 0;
  }

  return n;
}

bool Programme::has(const RoomUse use) const { return countOf(use) > 0; }

uint32_t Programme::find(const RoomUse use, const uint32_t instance) const
{
  for (uint32_t i = 0; i < rooms.size(); ++i)
  {
    if (rooms[i].use == use && rooms[i].instance == instance)
    {
      return i;
    }
  }

  return npos;
}

double areaLogLikelihood(const Programme &programme, const uint32_t room,
                         const double areaMetres)
{
  if (room >= programme.areaBinWeights.size())
  {
    return -std::numeric_limits<double>::infinity();
  }

  const std::vector<float> &weights = programme.areaBinWeights[room];
  const uint32_t bin = binOf(kAreaEdges, areaMetres / sqFtToSqM);

  if (bin >= weights.size() || !(weights[bin] > 0.f))
  {
    return -std::numeric_limits<double>::infinity();
  }

  // The bin's mass spread over its width, so the value returned is a density
  // and two rooms with different bin widths compare.
  //
  // Interpolated between bin centres rather than taken as the bin's constant.
  // A piecewise-constant density is flat inside a bin, so an optimiser
  // minimising its negative log has no reason to prefer any area within one -
  // and the bins here are 25 square feet wide near the bottom and 50 higher up,
  // which is most of a small room. Interpolation gives the search the gradient
  // the paper's continuous distributions would have had.
  const auto density = [&weights](const uint32_t index) {
    const double width = (kAreaEdges[index + 1] - kAreaEdges[index]) * sqFtToSqM;

    return double(weights[index]) / std::max(width, 1e-6);
  };

  const double sqFt = areaMetres / sqFtToSqM;
  const double centre = (kAreaEdges[bin] + kAreaEdges[bin + 1]) * .5;

  const bool towardNext = sqFt > centre;
  const uint32_t other = towardNext ? bin + 1 : (bin > 0 ? bin - 1 : bin);

  if (other >= weights.size() || other == bin)
  {
    return std::log(std::max(density(bin), 1e-12));
  }

  const double otherCentre = (kAreaEdges[other] + kAreaEdges[other + 1]) * .5;
  const double span = otherCentre - centre;
  const double t = std::abs(span) > 1e-9
                     ? std::min(1., std::max(0., (sqFt - centre) / span))
                     : 0.;

  const double here = std::max(density(bin), 1e-12);
  const double there = std::max(density(other), 1e-12);

  return std::log(here) * (1. - t) + std::log(there) * t;
}

double aspectLogLikelihood(const Programme &programme, const uint32_t room,
                           const double longOverShort)
{
  if (room >= programme.aspectBinWeights.size())
  {
    return -std::numeric_limits<double>::infinity();
  }

  const std::vector<float> &weights = programme.aspectBinWeights[room];
  const uint32_t state = programmeAspectState(longOverShort);

  if (state >= weights.size() || !(weights[state] > 0.f))
  {
    return -std::numeric_limits<double>::infinity();
  }

  return std::log(double(weights[state]));
}

bool sampleProgramme(const ProgrammeBrief &brief, const uint64_t seed, Programme &out)
{
  out = Programme{};
  out.building = brief.building;

  const Prior &p = prior(brief.building);

  if (!p.invalidWhy.empty())
  {
    return false;
  }

  const BayesianNetwork &net = activeProgrammeNetwork(brief.building);

  Evidence evidence;
  evidence.resize(net.variableCount());
  evidence.set(p.standing, int32_t(brief.standing));

  if (brief.storeys)
  {
    const uint32_t s = *brief.storeys;

    if (s < 1 || s > 4)
    {
      return false;
    }

    evidence.set(p.storeys, int32_t(s - 1));
  }

  if (brief.bedrooms)
  {
    if (*brief.bedrooms > 6)
    {
      return false;
    }

    evidence.set(p.bedroomCount, int32_t(*brief.bedrooms));
  }

  if (brief.totalAreaMetres)
  {
    evidence.set(p.totalArea, int32_t(binOf(kTotalEdges, *brief.totalAreaMetres / sqFtToSqM)));
  }

  // The lot is fixed and the programme has to fit on it. This clamp runs the
  // opposite way from the paper's, which took the footprint as given and never
  // sampled it - here the network owns the footprint, so the ground is
  // imposed by clamping the two variables that determine it.
  const bool footprintGiven = brief.footprintX && brief.footprintZ
                              && *brief.footprintX > 0 && *brief.footprintZ > 0;
  double givenX = 0, givenZ = 0;

  if (footprintGiven)
  {
    givenX = *brief.footprintX;
    givenZ = *brief.footprintZ;

    const double longSide = std::max(givenX, givenZ);
    const double shortSide = std::min(givenX, givenZ);

    // Nearest aspect state, not a bin lookup: the states are representative
    // ratios rather than the edges of a ladder.
    uint32_t best = 0;

    for (uint32_t i = 1; i < kAspects.size(); ++i)
    {
      if (std::abs(kAspects[i] - longSide / shortSide)
          < std::abs(kAspects[best] - longSide / shortSide))
      {
        best = i;
      }
    }

    evidence.set(p.footprintAspect, int32_t(best));

    if (!brief.totalAreaMetres)
    {
      // Total area follows from the ground and the storey count, so the
      // storey count has to be settled first. Draw it if the brief left it
      // open, then clamp both.
      uint32_t storeys = brief.storeys ? *brief.storeys : 0;

      if (storeys == 0)
      {
        Evidence probe;

        if (!net.sample(evidence, seed ^ 0x51ED270B6C1A9F2Dull, probe))
        {
          return false;
        }

        storeys = uint32_t(probe.at(p.storeys)) + 1;
        evidence.set(p.storeys, int32_t(storeys - 1));
      }

      const double impliedSqFt = givenX * givenZ * double(storeys) / sqFtToSqM;
      evidence.set(p.totalArea, int32_t(binOf(kTotalEdges, impliedSqFt)));
    }
  }

  Evidence drawn;

  if (!net.sample(evidence, seed, drawn))
  {
    return false;
  }

  Rng rng(seed ^ 0xA24BAED4963EE407ull);

  out.standing = brief.standing;
  out.storeys = uint32_t(drawn.at(p.storeys)) + 1;
  out.basement = drawn.at(p.basement) == 1;

  const uint32_t totalBin = uint32_t(drawn.at(p.totalArea));
  out.totalAreaMetres = representative(kTotalEdges, totalBin) * sqFtToSqM;

  if (footprintGiven)
  {
    out.footprintX = givenX;
    out.footprintZ = givenZ;
  }
  else
  {
    const double aspect = kAspects[uint32_t(drawn.at(p.footprintAspect))];
    const double groundArea = out.totalAreaMetres / double(out.storeys);

    out.footprintX = std::sqrt(groundArea * aspect);
    out.footprintZ = std::sqrt(groundArea / aspect);
  }

  // --- rooms ---

  const std::array<RoomPrior, roomUseCount> &priors = roomPriors();

  for (const RoomUse use : p.vocabulary)
  {
    const uint32_t u = uint32_t(use);

    uint32_t instances = 0;

    if (use == RoomUse::Bedroom)
    {
      instances = uint32_t(drawn.at(p.bedroomCount));
    }
    else if (use == RoomUse::Closet)
    {
      instances = uint32_t(drawn.at(p.closetCount));
    }
    else if (p.exists[u] == BayesianNetwork::invalid)
    {
      continue;
    }
    else if (use == RoomUse::Landing)
    {
      // One per upper floor. A stair passes through every floor it serves and
      // arrives at a landing on each of them; a single landing shared between
      // two floors leaves the upper one with no way in.
      instances = drawn.at(p.exists[u]) == 1 ? std::max(1u, out.storeys - 1) : 0;
    }
    else
    {
      instances = drawn.at(p.exists[u]) == 1 ? 1 : 0;
    }

    if (instances == 0)
    {
      continue;
    }

    // The conditional distribution over area bins this room was drawn from.
    // The paper carries one Area node per room *instance*; this network has
    // one per room *type*, so instance zero takes the sampled bin and the
    // rest are redrawn from the same row. That is a departure, and it is the
    // reason two bedrooms in one house come out different sizes.
    const std::vector<double> areaRow = net.conditionalRow(p.area[u], drawn);

    if (areaRow.empty())
    {
      return false;
    }

    std::vector<float> weights(areaRow.size());

    for (size_t i = 0; i < areaRow.size(); ++i)
    {
      weights[i] = float(areaRow[i]);
    }

    // The aspect distribution the network produced, kept whole. It is
    // conditioned on the area bin, so it is read for the bin each instance
    // actually drew.

    for (uint32_t instance = 0; instance < instances; ++instance)
    {
      const uint32_t bin =
        instance == 0 ? uint32_t(drawn.at(p.area[u])) : pick(areaRow, rng);

      Evidence forAspect = drawn;
      forAspect.set(p.area[u], int32_t(bin));

      const std::vector<double> aspectRow = net.conditionalRow(p.aspect[u], forAspect);
      const uint32_t aspectState =
        instance == 0 && bin == uint32_t(drawn.at(p.area[u]))
          ? uint32_t(drawn.at(p.aspect[u]))
          : pick(aspectRow, rng);

      RoomSpec room;
      room.use = use;
      room.instance = uint8_t(instance);
      room.frontsStreet =
        std::find(p.streetFacing.begin(), p.streetFacing.end(), use) != p.streetFacing.end();
      room.areaBin = uint8_t(bin);
      room.floor = floorFor(priors[u].band, out.storeys, out.basement, instance);

      // Uniform inside the bin, in square feet, because the bin is where the
      // network's belief stops and anything finer would be invented.
      const double lo = kAreaEdges[bin];
      const double hi = kAreaEdges[bin + 1];
      room.areaMetres = (lo + rng.unit() * (hi - lo)) * sqFtToSqM;
      room.aspect = kAspects[aspectState];

      std::vector<float> aspectWeights(aspectRow.size());

      for (size_t i = 0; i < aspectRow.size(); ++i)
      {
        aspectWeights[i] = float(aspectRow[i]);
      }

      out.rooms.push_back(room);
      out.areaBinWeights.push_back(weights);
      out.aspectBinWeights.push_back(std::move(aspectWeights));
    }
  }

  // --- the house-place ------------------------------------------------------
  //
  // Every dwelling has one room you could call the house: where the fire is,
  // where people sit, where they eat, and where - in a small house - most of
  // them sleep. A programme that drew a kitchen, a bedroom and no living room
  // describes a building nobody lives in, and on a small lot that comes out as
  // a house consisting of a kitchen.
  //
  // So if nothing of the living band was drawn, the largest service room
  // becomes it. This is not a repair of the geometry - the room was already
  // there and already had its area - it is the recognition that in a one- or
  // two-room house the kitchen *is* the house-place, which is what it was
  // called for most of the period: a cottage's single room cooked, ate, sat
  // and slept, and calling it a kitchen describes only a fifth of its use.
  // ...and only where the class has such a room to give. A prison's vocabulary
  // has no living room in it, and a rule that invents one is the same fault as
  // a cottage whose only room is called a kitchen, committed by the rule meant
  // to fix it.
  const ClassProfile &houseProfile = classProfile(brief.building);
  bool ownsLiving = houseProfile.rooms.empty();

  for (const RoomUse use : houseProfile.rooms)
  {
    ownsLiving = ownsLiving || use == RoomUse::LivingRoom;
  }

  if (houseProfile.dwelling && ownsLiving)
  {
    const auto isLiving = [](const RoomUse use) {
      return use == RoomUse::LivingRoom || use == RoomUse::Parlour || use == RoomUse::Hall
             || use == RoomUse::GreatHall || use == RoomUse::DiningRoom
             || use == RoomUse::DrawingRoom || use == RoomUse::MorningRoom
             || use == RoomUse::BreakfastRoom || use == RoomUse::Refectory
             || use == RoomUse::TapRoom;
    };

    bool lived = false;
    uint32_t enclosed = 0;

    for (const RoomSpec &room : out.rooms)
    {
      lived = lived || isLiving(room.use);
      enclosed += enclosesFloorArea(room.use) ? 1 : 0;
    }

    // Only in a small house. In a large one a kitchen has a scullery and a
    // larder hanging off it, and taking the kitchen away to make a parlour
    // strands both - the adjacency the network drew was to a kitchen, and
    // there is no longer one. A house of three rooms has nothing hanging off
    // anything, which is exactly the house this rule is for.
    if (!lived && enclosed <= 3)
    {
      uint32_t best = Programme::npos;

      for (uint32_t r = 0; r < out.rooms.size(); ++r)
      {
        const RoomUse use = out.rooms[r].use;

        if (use != RoomUse::Kitchen && use != RoomUse::Scullery && use != RoomUse::Shop
            && use != RoomUse::Workshop)
        {
          continue;
        }

        if (best == Programme::npos || out.rooms[r].areaMetres > out.rooms[best].areaMetres)
        {
          best = r;
        }
      }

      if (best != Programme::npos)
      {
        out.rooms[best].use = RoomUse::LivingRoom;

        // Its area was drawn for what it was, and a house-place is the biggest
        // room in the house: the draw stands, but it is not allowed to be
        // smaller than a room a family lives in.
        out.rooms[best].areaMetres = std::max(out.rooms[best].areaMetres, 13.);
      }
    }
    else if (!lived && brief.standing != PlotStanding::Slum
             && brief.standing != PlotStanding::Rural)
    {
      // A larger house with no living room gets one, rather than having its
      // kitchen renamed: in a house of six rooms the kitchen has a scullery
      // and a larder attached to it and is wanted as a kitchen. What is
      // missing is somewhere to sit, so that is what is added.
      //
      // Not on a holding, though. A farm kitchen with a scullery off it and no
      // parlour is not a house missing its parlour - it is a farmhouse, and
      // the kitchen is where the household sits. Adding a parlour to one is
      // the same mistake as calling a cottage's single room a kitchen, made in
      // the other direction.
      RoomSpec room;
      room.use = RoomUse::LivingRoom;
      room.floor = 0;
      room.areaMetres = 15.;
      out.rooms.push_back(room);

      std::vector<float> areaRow(kAreaEdges.size() - 1, 0.f);
      double mass = 0;

      for (size_t bin = 0; bin + 1 < kAreaEdges.size(); ++bin)
      {
        const double middle = (kAreaEdges[bin] + kAreaEdges[bin + 1]) * .5;
        const double t = (std::log(std::max(middle, 1.)) - std::log(170.)) / .4;
        const double weight = std::exp(-.5 * t * t) + 1e-3;

        areaRow[bin] = float(weight);
        mass += weight;
      }

      for (float &weight : areaRow)
      {
        weight = float(double(weight) / std::max(mass, 1e-9));
      }

      out.areaBinWeights.push_back(std::move(areaRow));
      out.aspectBinWeights.push_back({.30f, .28f, .22f, .14f, .06f});
    }
  }

  // --- a building with an upper floor has a way up to it ---------------------
  //
  // Not a probability. The network gives a poor two-storey house a landing at
  // two per cent, which is right about *rooms called landings* in the book's
  // plans and wrong about stairs: a floor you cannot reach is not a floor, and
  // §5 depends on the staircase being there - it is the multi-story space that
  // couples the optimisation across floors, and the paper's own failure case
  // is a stair "blocked on the second story".
  //
  // So where the draw left a floor unreachable, the space is added. A poor
  // house gets a ladder stair in the corner of the room below and no landing
  // worth the name, which is exactly what four square metres of staircase and
  // three of landing describe.
  if (out.storeys > 1)
  {
    const std::array<RoomPrior, roomUseCount> &priors = roomPriors();

    const auto add = [&](const RoomUse use, const int32_t floor, const double area) {
      RoomSpec room;
      room.use = use;
      room.floor = floor;
      room.areaMetres = area;
      out.rooms.push_back(room);

      // The distributions it would have been drawn from, so §5.2 scores it the
      // way it scores every other room rather than treating it as free.
      //
      // Spread, not a spike: the dimension cost is a negative log likelihood,
      // and a one-hot row makes every area but one infinitely expensive - which
      // does not constrain the optimiser, it silences it, because inf compares
      // equal to inf and no move ever looks better than another.
      const uint32_t u = uint32_t(use);
      const double sigma = std::max(priors[u].sigma, .2);
      const double centre = std::log(priors[u].typicalSqFt);

      std::vector<float> areaRow(kAreaEdges.size() - 1, 0.f);
      double mass = 0;

      for (size_t bin = 0; bin + 1 < kAreaEdges.size(); ++bin)
      {
        const double middle = (kAreaEdges[bin] + kAreaEdges[bin + 1]) * .5;
        const double t = (std::log(std::max(middle, 1.)) - centre) / sigma;
        const double weight = std::exp(-.5 * t * t) + 1e-3;

        areaRow[bin] = float(weight);
        mass += weight;
      }

      for (float &weight : areaRow)
      {
        weight = float(double(weight) / std::max(mass, 1e-9));
      }

      // Long and thin, as a staircase is, but not only that.
      std::vector<float> aspectRow(kAspects.size(), 0.f);
      const std::vector<double> shape{.12, .22, .28, .26, .12};

      for (size_t i = 0; i < aspectRow.size(); ++i)
      {
        aspectRow[i] = float(i < shape.size() ? shape[i] : .05);
      }

      out.areaBinWeights.push_back(std::move(areaRow));
      out.aspectBinWeights.push_back(std::move(aspectRow));
    };

    bool stair = false;

    for (const RoomSpec &room : out.rooms)
    {
      stair = stair || (room.use == RoomUse::Staircase && room.floor == 0);
    }

    if (!stair)
    {
      add(RoomUse::Staircase, 0, 4.2);
    }

    for (int32_t level = 1; level < int32_t(out.storeys); ++level)
    {
      bool arrives = false;

      for (const RoomSpec &room : out.rooms)
      {
        arrives = arrives
                  || ((room.use == RoomUse::Landing || room.use == RoomUse::Staircase)
                      && room.floor == level);
      }

      if (!arrives)
      {
        add(RoomUse::Landing, level, 3.4);
      }
    }
  }

  // --- adjacencies ---

  const std::vector<PairPrior> &pairs = p.pairs;

  // A cottage with no hall, no passage and no landing still has circulation:
  // it is the living rooms, and you walk through them. So in that house the
  // kitchen and the parlour serve every chamber, the way a landing would.
  const bool noCirculation = !out.has(RoomUse::Hall) && !out.has(RoomUse::Passage)
                             && !out.has(RoomUse::Landing);

  const auto serves = [noCirculation](const RoomUse use) {
    return isCirculation(use) || use == RoomUse::Yard
           || (noCirculation && (use == RoomUse::Kitchen || use == RoomUse::Parlour));
  };

  for (size_t i = 0; i < pairs.size(); ++i)
  {
    const uint32_t countA = out.countOf(pairs[i].a);
    const uint32_t countB = out.countOf(pairs[i].b);

    if (countA == 0 || countB == 0)
    {
      continue;
    }

    const bool door = drawn.at(p.pairNodes[i][1]) == 1;

    // A house with three bedrooms opens each of them onto the landing, and
    // whether it opens the second is not the same question as whether it
    // opened the first. The paper carries a node per room *instance* and so
    // asks the question once per bedroom; this network carries one per room
    // *type*, so the first instance takes the sampled node and the rest are
    // drawn here from the same probability.
    //
    // Deciding them all with one draw - which is what reusing the node does -
    // strands every bedroom in the house together whenever it misses, and
    // that was most of the doors the repair below had to put back.
    //
    // How many links there are is a different question from how many draws.
    // A landing serves every bedroom on its floor, so a circulation pair gets
    // one link per room; a dressing room serves the one bedroom it is off, so
    // an ordinary pair gets as many links as there are of the scarcer room.
    // Taking the larger count either way gave a mansion three bedrooms all
    // opening into the same dressing room.
    const uint32_t multiples = serves(pairs[i].a) || serves(pairs[i].b)
                                 ? std::max(countA, countB)
                                 : std::min(countA, countB);

    // The instances after the first are drawn here, so they have to be drawn
    // from the same conditional probability the node carries - including
    // whatever closes the pair, or a mansion's second bedroom would take a
    // kitchen door the first one was not allowed.
    bool closed = false;

    for (const RoomUse closer : pairs[i].closedBy)
    {
      closed = closed || out.has(closer);
    }

    const double adjacentProbability = closed ? pairs[i].whenClosed : pairs[i].adjacent;

    for (uint32_t k = 0; k < multiples; ++k)
    {
      const bool adjacent =
        k == 0 ? drawn.at(p.pairNodes[i][0]) == 1 : rng.unit() < adjacentProbability;

      if (!adjacent)
      {
        continue;
      }

      // The room there are more of leads, and the other follows it to its
      // floor: six bedroom doors have to find two landings, not the reverse.
      const bool aLeads = countA >= countB;
      const uint32_t lead =
        out.find(aLeads ? pairs[i].a : pairs[i].b, std::min(k, std::max(countA, countB) - 1));

      if (lead == Programme::npos)
      {
        continue;
      }

      const uint32_t follow = instanceReaching(out, aLeads ? pairs[i].b : pairs[i].a,
                                               aLeads ? countB : countA, k,
                                               out.rooms[lead].floor);

      const uint32_t a = aLeads ? lead : follow;
      const uint32_t b = aLeads ? follow : lead;

      if (a == Programme::npos || b == Programme::npos || a == b
          || !floorsMeet(out.rooms[a], out.rooms[b]))
      {
        continue;
      }

      out.links.push_back({a, b, door});
    }
  }

  connectProgramme(out, pairs);

  return !out.rooms.empty();
}

}  // namespace procedural::world::generator
