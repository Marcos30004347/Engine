#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "procedural/learn/BayesianNetwork.hpp"
#include "procedural/world/generator/PlotPlanner.hpp"

namespace procedural::world::generator {

// The architectural programme: what rooms a house has, how big they are, and
// which of them open into which.
//
// Merrell, Schkufza and Koltun's "Computer-Generated Residential Building
// Layouts" (SIGGRAPH Asia 2010) splits house generation in two, and the split
// is the whole idea. First the *programme* - a list of rooms with areas and
// aspect ratios and an adjacency graph over them - sampled from a Bayesian
// network fitted to real plans. Only then the geometry, which is an
// optimisation trying to realise that programme on the given ground.
//
// The existing dwelling planner does the opposite: it cuts the footprint into
// pieces and labels the pieces afterwards. That produces a partition, and a
// partition is not a plan - nothing in it knows that a scullery wants to be
// off the kitchen, or that you should not reach a bedroom through the dining
// room. The programme is where that knowledge lives.
//
// This file is the programme and the sampler for it. The network behind it is
// hand-authored (stage B0 of the plan): the structure comes from how these
// houses actually work and from what the corpus of 19th-century English
// builder's plates shows about which rooms co-occur, and the numbers are
// priors rather than fitted counts. That is deliberate and it is a working
// generator on its own; stage B1 replaces the numbers by structure learning
// over the transcribed plates without this interface changing.
//
// Units are metric here and imperial in the corpus. The plates are drawn in
// feet and inches and rounding twice loses the six-inch increments that carry
// the proportions, so the transcription keeps feet and the conversion happens
// exactly once, on the way in.
enum class RoomUse : uint8_t
{
  // Service. The whole point of these plans: an English house of this period
  // is a polite front and a service wing, and the wing is most of the rooms.
  Kitchen,
  Scullery,
  Larder,
  Pantry,
  Dairy,
  Washhouse,
  Cellar,

  // Polite.
  Parlour,

  // The room a cottage lives in, which is not a parlour: the parlour is the
  // front room kept for visitors and the living room is the one with the fire
  // in it. The corpus separates them on thirteen plates, which is what earns
  // this a slot of its own - the rule being a room type is worth a variable
  // when about eight plates carry it.
  LivingRoom,

  DiningRoom,
  DrawingRoom,
  BreakfastRoom,
  Study,
  Library,

  // Circulation. These are rooms, not gaps between rooms - the corpus names
  // them on the plates (hall 115 plates, passage 45, landing 28) and the
  // adjacency graph is supposed to route through them. A plan whose rooms
  // connect to each other directly is a plan you cross a bedroom to walk.
  Hall,
  Passage,
  Landing,
  Staircase,
  Lobby,
  Porch,

  // Private.
  Bedroom,
  DressingRoom,
  Nursery,
  Closet,
  WaterCloset,
  Bathroom,

  // Working.
  Office,
  Shop,
  Workshop,
  Store,
  Yard,

  // Rooms a large house has and a cottage does not. They sit here rather than
  // with the polite rooms because they only ever appear at the top of the
  // scale, and a vocabulary is easier to read when the rare end is together.
  MorningRoom,
  Boudoir,
  Conservatory,
  Veranda,
  ServantsHall,
  Brewhouse,

  // A public house. The bar is the plan - everything else is arranged so that
  // nobody can enter or leave without passing within sight of it.
  Bar,
  TapRoom,
  ClubRoom,
  CoffeeRoom,
  BilliardRoom,

  // A school, and a dispensary. Neither is a dwelling, and both have one
  // attached, which is why they are here at all.
  SchoolRoom,
  Dispensary,
  WaitingRoom,
  ConsultingRoom,

  // A farmstead's working buildings, which stand round the yard rather than
  // in the house.
  Stable,
  Barn,
  Loft,

  // A church. The nave and the chancel are one room divided by a screen and
  // by who may stand where, which is the whole plan: everything else hangs
  // off that division.
  Nave,
  Chancel,
  Aisle,
  Transept,
  Sanctuary,
  Narthex,
  SideChapel,
  Vestry,
  Crypt,
  Belfry,
  Cloister,
  ChapterHouse,

  // A gallery runs round the inside of a preaching box and round the inside
  // of a prison, for the same reason: it doubles the number of people who can
  // see one point.
  Gallery,

  // Castle and medieval hall. The great hall is not a big hall - it is a
  // different room, open to its roof, with the household eating in it and the
  // service rooms at one end behind a screen.
  GreatHall,
  ScreensPassage,
  Solar,
  Buttery,
  Keep,
  Gatehouse,
  Guardroom,
  Barracks,
  Armoury,
  Dungeon,
  Tower,
  Courtyard,
  Well,

  // A prison.
  Cell,
  Infirmary,
  ExerciseYard,

  // The state rooms of a mansion, which a large house does not have and which
  // are the reason a mansion is a different building rather than a big one.
  Saloon,
  Ballroom,
  SmokingRoom,
  GunRoom,

  // A mansion's service establishment, which is a household of its own.
  StillRoom,
  HousekeepersRoom,
  ButlersRoom,

  // Trade. A counting house is where the books are kept and the merchant sits;
  // a strong room is where the money is; an undercroft is the vaulted storey
  // under a hall, which a guild lets out as shops and a merchant uses himself.
  CountingHouse,
  StrongRoom,
  CouncilChamber,
  Undercroft,

  // The medieval countryside, which is most of the buildings there are.
  //
  // A longhouse is one range under one roof with the family at one end and the
  // beasts at the other, a cross passage between them, and the byre floor laid
  // to fall away from the living end. It is not a cottage with a shed: the
  // animals are in the building and their warmth is part of how it is heated.
  Byre,
  Croft,   // The worked ground behind the house: vegetables, not lawn.
  Garden,  // ...and the ground kept for pleasure, which is a wealthy idea.
  Orchard,
  Pen,     // Hurdled ground for stock, against the yard.
  Midden,

  // Housing people in numbers: an almshouse, a guild's lodging for its
  // journeymen, a mill's row, a barrack of farm servants. One room of beds and
  // a room to eat in, repeated.
  Dormitory,
  Refectory,

  Count
};

// What kind of building a programme is for.
//
// The paper fits three networks rather than one - single-, two- and
// three-storey houses - because "the structure of residential architectural
// programs varies significantly with the number of floors", and a hidden
// variable spanning that much variation needs more data than exists. The same
// argument applies with more force across building types: a tavern and a
// labourer's cottage share a kitchen and nothing else, and one network holding
// both would spend most of its probability mass saying that taverns have no
// bedrooms and cottages have no bar.
//
// So each class has its own network over its own vocabulary. Only the dwelling
// has enough transcribed examples to fit; the rest are hand-authored, which is
// what the corpus allows and is still a working generator.
enum class BuildingClass : uint8_t
{
  Dwelling,   // A house. The corpus's main subject and the only fitted one.
  ShopHouse,  // A shop on the street with the living accommodation over it.
  Inn,        // A tavern: public rooms arranged about a bar.
  School,     // School rooms with the master's residence attached.
  Dispensary, // Public rooms with a residence over them.
  Warehouse,  // Open floors on columns; not a dwelling at all.
  Farmstead,  // A house with a range of offices and working buildings.

  // Ecclesiastical. A church is a parish church with a chancel; a chapel is a
  // dissenting preaching box, which is a different building with the same
  // purpose - the book specifies both, with their fittings, on PDF page 600.
  Church,
  Chapel,
  Temple,

  // Medieval and defensive.
  Castle,
  ManorHouse,

  // Institutional.
  Prison,

  // A mansion is not a large house. It has state rooms a house does not, and
  // a service establishment run as a household of its own.
  Mansion,

  // Trade.
  GuildHall,
  CommerceHouse,

  // The two ends of the medieval scale, which is where most of a settlement
  // actually lives.
  //
  // A peasant's longhouse: one range, the family at one end and the beasts at
  // the other, a cross passage between. Almost every building in a village is
  // one of these, and none of the classes above describes it - they all assume
  // a household with rooms to spare.
  PeasantCottage,

  // ...and a lodging for people housed in numbers: a guild's journeymen, a
  // mill's hands, an almshouse. Rooms of beds and a room to eat in, repeated.
  Dormitory,

  Count
};

const char *name(PlotStanding);

const char *name(BuildingClass);

const char *name(RoomUse);

// Whether a room of this use is inside the house. Everything is, except the
// yard - which the plates draw and label, and which the adjacency graph needs
// because the back door opens onto it, but which is not floor area and must
// not be counted as any.
bool enclosesFloorArea(RoomUse);

// One room the programme asks for.
struct RoomSpec
{
  RoomUse use = RoomUse::Kitchen;

  // 0 is the ground floor; -1 a basement. A basement is a floor of the house
  // in these plans, not a cellar under it - page 27 of the corpus is a
  // basement plan with the kitchen, scullery, larder and dairy all in it.
  int32_t floor = 0;

  // Sampled, not averaged. The distribution it came from is kept in the
  // programme (see `areaBinWeights`) because the floor-plan optimiser scores
  // a plan by the log-likelihood of its room areas, and a single number per
  // room throws away exactly what it needs.
  double areaMetres = 0;

  // Long side over short, so always >= 1.
  double aspect = 1;

  // Which bin of the area distribution was drawn. Index into
  // `programmeAreaBinEdgesSqFt()`.
  uint8_t areaBin = 0;

  // Instances of the same use are numbered from zero: bedroom 0, bedroom 1.
  uint8_t instance = 0;

  // Whether this room has to be on the street.
  //
  // A shop is not a room that happens to face the street; facing the street is
  // what makes it a shop. A merchant's trading window, a guild's undercroft
  // shops and an inn's tap room are the same: put them at the back and the
  // building stops working. The planner anchors these to the street edge of
  // the footprint before it places anything else.
  bool frontsStreet = false;
};

// Direct access between two rooms: a door, or an opening with no door in it.
// The paper's definition, and not "shares a wall" - two rooms can share a wall
// and have nothing to do with each other, and the difference is the entire
// content of a plan.
struct Adjacency
{
  uint32_t a = 0, b = 0;
  bool door = true;  // false is an open wall.
};

struct Programme
{
  std::vector<RoomSpec> rooms;
  std::vector<Adjacency> links;

  // Per room, the distributions over area bins and aspect states it was drawn
  // from. Parallel to `rooms`.
  //
  // The paper is explicit that these are kept rather than collapsed: "while it
  // is possible to sample a single value for each room's area and aspect ratio
  // from the Bayesian network, we have found it advantageous to retain the
  // complete probability distribution produced for each of these values by the
  // network. Thus, for each room, we have distributions over possible areas and
  // aspect ratios. These distributions are used to penalize unlikely room
  // dimensions." §5.2's dimension cost is the negative log-likelihood under
  // exactly these, and a target value cannot stand in for them.
  std::vector<std::vector<float>> areaBinWeights;
  std::vector<std::vector<float>> aspectBinWeights;

  double totalAreaMetres = 0;

  // The network's own Footprint variable, in metres. This is the hook that
  // lets a programme drive the *exterior* as well as the interior: the massing
  // can take its outline from here rather than from a coverage fraction of the
  // lot. The paper took the footprint as given; we generate it.
  double footprintX = 0, footprintZ = 0;

  BuildingClass building = BuildingClass::Dwelling;

  // What the household could afford, which is the brief's and is kept because
  // everything downstream wants it: how many people live here, what the rooms
  // are furnished with, what the walls are made of.
  PlotStanding standing = PlotStanding::Modest;

  // Storeys above ground. A basement, if there is one, is floor -1 and is not
  // counted here.
  uint32_t storeys = 1;
  bool basement = false;

  // How many of `links` the sampler had to add itself, because the network
  // drew a room with no way into it.
  //
  // This is a diagnostic and it is meant to be small. Every adjacency in this
  // network is drawn independently given the rooms, so a house of thirty
  // rooms will strand one of them now and then however good the priors are -
  // but a large number here says the adjacency probabilities are too low, or
  // that a room has been given no partner to hang off. Watch it when the
  // priors change.
  uint32_t repairedLinks = 0;

  uint32_t countOf(RoomUse) const;
  bool has(RoomUse) const;

  // Index of the n-th room of a use, or npos.
  static constexpr uint32_t npos = 0xFFFFFFFFu;
  uint32_t find(RoomUse, uint32_t instance = 0) const;
};

// Anything set is clamped and the rest is sampled - the paper's own usage:
// "the user can specify a number of bedrooms and bathrooms, total square
// footage, areas of specific rooms", and the network fills the rest in
// conditionally.
struct ProgrammeBrief
{
  BuildingClass building = BuildingClass::Dwelling;
  PlotStanding standing = PlotStanding::Modest;

  std::optional<uint32_t> storeys;
  std::optional<uint32_t> bedrooms;
  std::optional<double> totalAreaMetres;

  // What the lot will allow. When these are set the clamp runs the other way
  // from the paper's: the ground is fixed and the network picks a programme
  // that fits on it.
  std::optional<double> footprintX, footprintZ;
};

// Samples a programme. False when the brief asks for something the network
// gives probability zero - a three-storey slum cottage, say. It returns false
// rather than quietly relaxing the brief, because a silently-relaxed brief is
// a house that does not fit its lot.
bool sampleProgramme(const ProgrammeBrief &, uint64_t seed, Programme &out);

// Log-likelihood of a room of this programme having the given area, under the
// distribution it was actually drawn from. This is what §5.2's cost function
// scores a candidate plan with; the sampled area is a draw from it and not the
// target.
double areaLogLikelihood(const Programme &, uint32_t room, double areaMetres);

// ...and of it having the given ratio of long side to short. The other half of
// §5.2's dimension term.
double aspectLogLikelihood(const Programme &, uint32_t room, double longOverShort);

// --- the network itself, exposed for the tests and the report ------------

// Bin edges for room area, in square feet, per the plan: 25 sq ft steps to
// 200, then 50 to 600, then one tail bin. The last edge is the open end.
const std::vector<double> &programmeAreaBinEdgesSqFt();

// Aspect-ratio states, as the representative ratio of each: 1:1, 5:4, 3:2,
// 2:1 and a tail.
const std::vector<double> &programmeAspectStates();

// Whole-house square footage bins, and which bin a figure falls in. The bins
// are coarse because there are only ever a few hundred plates to fit them from.
const std::vector<double> &programmeTotalAreaBinEdgesSqFt();
uint32_t programmeTotalAreaBin(double squareFeet);
uint32_t programmeAreaBin(double squareFeet);

// Which aspect state a ratio of long side to short is nearest.
uint32_t programmeAspectState(double longOverShort);

// The hand-authored network for a class, built once. Stage B0. Its node names
// are stable and are what a fitted network will have to reproduce.
const learn::BayesianNetwork &priorProgrammeNetwork(BuildingClass = BuildingClass::Dwelling);

// The rooms and the room pairs a class's network is built over. A room absent
// from the vocabulary has no node at all, which is the point: a tavern's
// network should not carry a nursery.
const std::vector<RoomUse> &programmeVocabulary(BuildingClass);
const std::vector<std::pair<RoomUse, RoomUse>> &programmeAdjacencyPairs(BuildingClass);

// The network `sampleProgramme` actually draws from: the hand-authored one
// until a fitted network is installed over it.
//
// This is a process-wide switch, and deliberately so - the generator should be
// sampling from one network rather than from whichever the caller happened to
// build. Passing null puts the prior back.
void installProgrammeNetwork(const learn::BayesianNetwork *,
                             BuildingClass = BuildingClass::Dwelling);
const learn::BayesianNetwork &activeProgrammeNetwork(BuildingClass = BuildingClass::Dwelling);

// Node-name helpers, so a test can clamp by meaning rather than by index.
std::string existsNodeName(RoomUse);
std::string areaNodeName(RoomUse);
std::string aspectNodeName(RoomUse);
std::string adjacencyNodeName(RoomUse a, RoomUse b);
std::string adjacencyTypeNodeName(RoomUse a, RoomUse b);

// The dwelling's room pairs. A pair that is not here is a pair the programme
// will never join directly, which is how the privacy gradient is stated:
// there is no bedroom-bathroom node, so no plan can ask for that door.
const std::vector<std::pair<RoomUse, RoomUse>> &programmeAdjacencyPairs();

}  // namespace procedural::world::generator
