#include "procedural/world/generator/ProgrammeFit.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <cmath>
#include <map>

namespace procedural::world::generator {

using learn::BayesianNetwork;
using learn::Evidence;

namespace {

constexpr double sqFtToSqM = 0.09290304;

bool contains(const std::string &haystack, const char *needle)
{
  return haystack.find(needle) != std::string::npos;
}

// Every node of the network this fitter needs to address, found once by name.
struct Handles
{
  uint32_t standing = BayesianNetwork::invalid;
  uint32_t storeys = BayesianNetwork::invalid;
  uint32_t basement = BayesianNetwork::invalid;
  uint32_t totalArea = BayesianNetwork::invalid;
  uint32_t footprintAspect = BayesianNetwork::invalid;
  uint32_t bedroomCount = BayesianNetwork::invalid;
  uint32_t closetCount = BayesianNetwork::invalid;

  std::vector<uint32_t> exists, area, aspect;
  std::vector<uint32_t> adjacency, link;

  explicit Handles(const BayesianNetwork &net)
  {
    // A class's network carries only its own rooms and pairs, so every lookup
    // may legitimately come back invalid. Nothing below assumes otherwise.
    standing = net.find("Standing");
    storeys = net.find("Storeys");
    basement = net.find("BasementExists");
    totalArea = net.find("TotalArea");
    footprintAspect = net.find("FootprintAspect");
    bedroomCount = net.find("BedroomCount");
    closetCount = net.find("ClosetCount");

    for (uint32_t u = 0; u < uint32_t(RoomUse::Count); ++u)
    {
      const RoomUse use = RoomUse(u);
      exists.push_back(net.find(existsNodeName(use)));
      area.push_back(net.find(areaNodeName(use)));
      aspect.push_back(net.find(aspectNodeName(use)));
    }

    for (const auto &pair : programmeAdjacencyPairs())
    {
      adjacency.push_back(net.find(adjacencyNodeName(pair.first, pair.second)));
      link.push_back(net.find(adjacencyTypeNodeName(pair.first, pair.second)));
    }
  }

  static bool known(const uint32_t node) { return node != BayesianNetwork::invalid; }
};

// Whether the record described the whole house, floor by floor. Only then does
// a room's *absence* mean anything.
bool describedEveryFloor(const CorpusProgramme &programme)
{
  if (programme.basement && !programme.drewFloor(-1))
  {
    return false;
  }

  for (int32_t level = 0; level < int32_t(programme.storeys); ++level)
  {
    if (!programme.drewFloor(level))
    {
      return false;
    }
  }

  return true;
}

}  // namespace

bool standingOfKind(const std::string &kind, PlotStanding &out)
{
  // A judgement, written down rather than buried. The corpus names building
  // types and the generator names social standings, and something has to join
  // them; this is the join, and it is the first thing to question if fitted
  // houses come out at the wrong scale.
  //
  // Order matters - the first match wins - so the specific names come before
  // the general ones that would swallow them.
  static const std::vector<std::pair<const char *, PlotStanding>> table = {
    // Grand: the largest houses in the book.
    {"first class", PlotStanding::Grand},
    {"first-rate", PlotStanding::Grand},
    {"mansion", PlotStanding::Grand},
    {"gentleman", PlotStanding::Grand},
    {"italian villa", PlotStanding::Grand},

    // Rural: a house with land attached.
    {"farm", PlotStanding::Rural},
    {"country house", PlotStanding::Rural},
    {"bailiff", PlotStanding::Rural},

    // Slum: the smallest, and the ones let by the week.
    {"workman", PlotStanding::Slum},
    {"flat", PlotStanding::Slum},

    // Modest: a labouring or artisan household.
    {"labourer", PlotStanding::Modest},
    {"model cottage", PlotStanding::Modest},
    {"lodge", PlotStanding::Modest},
    {"six-roomed", PlotStanding::Modest},
    {"fourth-rate", PlotStanding::Modest},
    {"shop", PlotStanding::Modest},

    // Wealthy: town houses and the larger villas.
    {"town house", PlotStanding::Wealthy},
    {"town residence", PlotStanding::Wealthy},
    {"second-rate", PlotStanding::Wealthy},
    {"ornamental villa", PlotStanding::Wealthy},
    {"isolated villa", PlotStanding::Wealthy},
    {"family residence", PlotStanding::Wealthy},
    {"nine-roomed", PlotStanding::Wealthy},
    {"eight-roomed", PlotStanding::Wealthy},

    // Comfortable: everything in between, which is most of the book.
    {"third-rate", PlotStanding::Comfortable},
    {"seven-room", PlotStanding::Comfortable},
    {"suburban", PlotStanding::Comfortable},
    {"villa", PlotStanding::Comfortable},
    {"cottage residence", PlotStanding::Comfortable},
    {"cottage", PlotStanding::Modest},
    {"residence", PlotStanding::Comfortable},
    {"house", PlotStanding::Comfortable},
  };

  std::string lowered;

  for (const char ch : kind)
  {
    lowered.push_back(char(std::tolower(ch)));
  }

  for (const auto &entry : table)
  {
    if (contains(lowered, entry.first))
    {
      out = entry.second;

      return true;
    }
  }

  return false;
}

BuildingClass buildingClassOfKind(const std::string &kind)
{
  std::string lowered;

  for (const char ch : kind)
  {
    lowered.push_back(char(std::tolower(ch)));
  }

  // Order matters: "shop and residence" is a shop house, and "farm house" is a
  // farmstead even though both contain the word house.
  if (contains(lowered, "tavern") || contains(lowered, "inn"))
  {
    return BuildingClass::Inn;
  }

  if (contains(lowered, "school"))
  {
    return BuildingClass::School;
  }

  if (contains(lowered, "dispensary"))
  {
    return BuildingClass::Dispensary;
  }

  if (contains(lowered, "warehouse") || contains(lowered, "factory"))
  {
    return BuildingClass::Warehouse;
  }

  if (contains(lowered, "chapel") || contains(lowered, "meeting house"))
  {
    return BuildingClass::Chapel;
  }

  if (contains(lowered, "church") || contains(lowered, "cathedral"))
  {
    return BuildingClass::Church;
  }

  if (contains(lowered, "temple"))
  {
    return BuildingClass::Temple;
  }

  if (contains(lowered, "castle") || contains(lowered, "keep"))
  {
    return BuildingClass::Castle;
  }

  if (contains(lowered, "prison") || contains(lowered, "gaol")
      || contains(lowered, "bridewell") || contains(lowered, "house of correction"))
  {
    return BuildingClass::Prison;
  }

  if (contains(lowered, "manor") || contains(lowered, "medieval"))
  {
    return BuildingClass::ManorHouse;
  }

  // A mansion is its own class; "first class town mansion" and "gentleman's
  // residence" are the book's names for it.
  if (contains(lowered, "mansion") || contains(lowered, "gentleman"))
  {
    return BuildingClass::Mansion;
  }

  if (contains(lowered, "farm"))
  {
    return BuildingClass::Farmstead;
  }

  if (contains(lowered, "shop"))
  {
    return BuildingClass::ShopHouse;
  }

  return BuildingClass::Dwelling;
}

Evidence evidenceOf(const CorpusProgramme &programme, const BayesianNetwork &net,
                    const BuildingClass klass)
{
  const Handles h(net);
  (void)klass;

  Evidence evidence;
  evidence.resize(net.variableCount());

  if (!programme.dwelling)
  {
    return evidence;
  }

  PlotStanding standing = PlotStanding::Modest;

  if (!standingOfKind(programme.kind, standing))
  {
    return evidence;
  }

  evidence.set(h.standing, int32_t(standing));
  evidence.set(h.storeys, int32_t(std::min(programme.storeys, 3u)) - 1);
  evidence.set(h.basement, programme.basement ? 1 : 0);

  const bool whole = describedEveryFloor(programme);

  // --- the global variables ---

  if (programme.massing.known())
  {
    const double total = programme.massing.areaSqFt() * double(programme.storeys);
    evidence.set(h.totalArea, int32_t(programmeTotalAreaBin(total)));

    const double longSide = std::max(programme.massing.frontageFeet, programme.massing.depthFeet);
    const double shortSide = std::min(programme.massing.frontageFeet, programme.massing.depthFeet);
    evidence.set(h.footprintAspect, int32_t(programmeAspectState(longSide / shortSide)));
  }
  else if (whole)
  {
    // No stated footprint, but the whole house was described - so the sum of
    // its dimensioned rooms is a floor under the total, and a usable one when
    // most of the rooms carry a dimension.
    double sum = 0;
    uint32_t dimensioned = 0, enclosed = 0;

    for (const CorpusRoom &room : programme.rooms)
    {
      if (room.mapped != RoomUse::Count && !enclosesFloorArea(room.mapped))
      {
        continue;
      }

      ++enclosed;

      if (room.dimensioned)
      {
        ++dimensioned;
        sum += room.feetLong * room.feetShort;
      }
    }

    if (enclosed > 0 && dimensioned * 4 >= enclosed * 3)
    {
      // Three quarters of the rooms measured; scale the rest in at the mean,
      // and add a fifth for the walls the plan draws and the labels do not.
      const double scaled = sum * double(enclosed) / double(dimensioned) * 1.2;
      evidence.set(h.totalArea, int32_t(programmeTotalAreaBin(scaled)));
    }
  }

  // --- counted rooms ---

  uint32_t bedrooms = 0, closets = 0;

  for (const CorpusRoom &room : programme.rooms)
  {
    bedrooms += room.mapped == RoomUse::Bedroom ? 1 : 0;
    closets += room.mapped == RoomUse::Closet ? 1 : 0;
  }

  if (Handles::known(h.bedroomCount) && (whole || bedrooms > 0))
  {
    evidence.set(h.bedroomCount, int32_t(std::min(bedrooms, 10u)));
  }

  if (Handles::known(h.closetCount) && (whole || closets > 0))
  {
    evidence.set(h.closetCount, int32_t(std::min(closets, 4u)));
  }

  // --- existence, area and aspect ---

  for (uint32_t u = 0; u < uint32_t(RoomUse::Count); ++u)
  {
    const RoomUse use = RoomUse(u);

    const CorpusRoom *first = nullptr;
    const CorpusRoom *dimensioned = nullptr;

    for (const CorpusRoom &room : programme.rooms)
    {
      if (room.mapped != use)
      {
        continue;
      }

      first = first == nullptr ? &room : first;
      dimensioned = dimensioned == nullptr && room.dimensioned ? &room : dimensioned;
    }

    if (!Handles::known(h.exists[u]))
    {
      // This class's network has no such room, so the record has nothing to
      // say about it here.
      continue;
    }

    if (first != nullptr)
    {
      evidence.set(h.exists[u], 1);
    }
    else if (whole)
    {
      // A room's absence is evidence only when the whole house was described.
      evidence.set(h.exists[u], 0);
    }

    if (dimensioned != nullptr && Handles::known(h.area[u]))
    {
      const double sqFt = dimensioned->feetLong * dimensioned->feetShort;
      evidence.set(h.area[u], int32_t(programmeAreaBin(sqFt)));
      evidence.set(h.aspect[u], int32_t(programmeAspectState(dimensioned->aspect)));
    }
  }

  // --- adjacencies ---

  // Absence of a door is evidence only where the doors were actually got
  // rather than worked out: named in the prose, or read off the plate as
  // breaks in the wall. A record whose adjacencies were inferred from which
  // rooms adjoin recorded the doors it was sure of and said nothing about the
  // rest, and reading that silence as a wall would teach the network that
  // these houses have hardly any doors in them.
  const bool doorsLegible = programme.adjacencySource == "stated"
                            || programme.adjacencySource == "openings";
  const auto &pairs = programmeAdjacencyPairs();

  for (size_t i = 0; i < pairs.size(); ++i)
  {
    if (!Handles::known(h.adjacency[i]))
    {
      continue;
    }

    bool joined = false;

    for (const CorpusLink &link : programme.links)
    {
      const RoomUse a = programme.rooms[link.a].mapped;
      const RoomUse b = programme.rooms[link.b].mapped;

      if ((a == pairs[i].first && b == pairs[i].second)
          || (a == pairs[i].second && b == pairs[i].first))
      {
        joined = true;
        if (Handles::known(h.link[i]))
        {
          evidence.set(h.link[i], link.door ? 1 : 0);
        }

        break;
      }
    }

    const uint32_t ea = h.exists[uint32_t(pairs[i].first)];
    const uint32_t eb = h.exists[uint32_t(pairs[i].second)];

    if (!Handles::known(ea) || !Handles::known(eb))
    {
      continue;
    }

    const bool bothPresent = evidence.at(ea) == 1 && evidence.at(eb) == 1;

    if (joined)
    {
      evidence.set(h.adjacency[i], 1);
    }
    else if (bothPresent && doorsLegible)
    {
      // Both rooms there, the doors legible, and no link drawn between them.
      evidence.set(h.adjacency[i], 0);
    }
    else if (evidence.known(ea) && evidence.known(eb) && !bothPresent)
    {
      // One of them is not in the house, so the pair cannot be adjacent. This
      // is a certainty rather than an observation, but counting it keeps the
      // impossible rows of the table from drifting off their hard zero.
      evidence.set(h.adjacency[i], 0);
    }
  }

  return evidence;
}

namespace {

// Log-likelihood of the observed part of an assignment: the sum over nodes
// that are observed and whose parents are all observed.
double observedLogLikelihood(const BayesianNetwork &net, const Evidence &evidence)
{
  double total = 0;

  for (uint32_t v = 0; v < net.variableCount(); ++v)
  {
    if (!evidence.known(v))
    {
      continue;
    }

    const uint32_t index = net.rowIndexOf(v, evidence);

    if (index == BayesianNetwork::invalid)
    {
      continue;
    }

    const std::vector<double> row = net.row(v, index);
    const double p = row[size_t(evidence.at(v))];

    total += std::log(std::max(p, 1e-12));
  }

  return total;
}

}  // namespace

bool fitProgrammeNetwork(const std::vector<CorpusProgramme> &corpus,
                         const double priorStrength,
                         BayesianNetwork &out,
                         FitReport &report,
                         std::string &why,
                         const BuildingClass klass)
{
  why.clear();
  report = FitReport{};

  if (corpus.empty())
  {
    why = "an empty corpus";

    return false;
  }

  if (!(priorStrength > 0))
  {
    why = "the prior strength must be positive; zero would make every unseen "
          "row uniform, which is a stronger claim than the prior it replaces";

    return false;
  }

  const BayesianNetwork &prior = priorProgrammeNetwork(klass);
  out = prior;

  report.observations.assign(prior.variableCount(), 0);

  // counts[node][row][state]
  std::vector<std::vector<std::vector<double>>> counts(prior.variableCount());

  for (uint32_t v = 0; v < prior.variableCount(); ++v)
  {
    counts[v].assign(prior.rowCount(v),
                     std::vector<double>(prior.variableAt(v).states.size(), 0.));
  }

  std::vector<Evidence> observations;
  std::map<std::string, bool> unplaced;

  for (const CorpusProgramme &programme : corpus)
  {
    // A record trains its own class and no other. A tavern says nothing about
    // how a cottage is arranged, and counting it into the dwelling network is
    // how a network learns that houses sometimes have a bar.
    if (buildingClassOfKind(programme.kind) != klass)
    {
      continue;
    }

    PlotStanding standing = PlotStanding::Modest;

    if (!standingOfKind(programme.kind, standing))
    {
      unplaced[programme.kind] = true;

      continue;
    }

    const Evidence evidence = evidenceOf(programme, prior, klass);
    observations.push_back(evidence);
    ++report.programmes;

    for (uint32_t v = 0; v < prior.variableCount(); ++v)
    {
      if (!evidence.known(v))
      {
        continue;
      }

      const uint32_t index = prior.rowIndexOf(v, evidence);

      if (index == BayesianNetwork::invalid)
      {
        continue;
      }

      counts[v][index][size_t(evidence.at(v))] += 1.;
      ++report.observations[v];
      ++report.totalObservations;
    }
  }

  for (const auto &entry : unplaced)
  {
    report.unplaced.push_back(entry.first);
  }

  // The prior's own numbers become pseudo-counts, and the corpus adds to them.
  for (uint32_t v = 0; v < prior.variableCount(); ++v)
  {
    report.nodesWithEvidence += report.observations[v] > 0 ? 1 : 0;

    for (uint32_t r = 0; r < prior.rowCount(v); ++r)
    {
      const std::vector<double> priorRow = prior.row(v, r);
      std::vector<double> fitted(priorRow.size(), 0.);

      for (size_t s = 0; s < priorRow.size(); ++s)
      {
        fitted[s] = priorRow[s] * priorStrength + counts[v][r][s];
      }

      if (!out.setRow(v, r, fitted))
      {
        why = "could not write row " + std::to_string(r) + " of "
              + prior.variableAt(v).name;

        return false;
      }
    }
  }

  std::string invalid;

  if (!out.validate(invalid))
  {
    why = "the fitted network does not validate: " + invalid;

    return false;
  }

  for (const Evidence &evidence : observations)
  {
    report.priorLogLikelihood += observedLogLikelihood(prior, evidence);
    report.fittedLogLikelihood += observedLogLikelihood(out, evidence);
  }

  if (!observations.empty())
  {
    report.priorLogLikelihood /= double(observations.size());
    report.fittedLogLikelihood /= double(observations.size());
  }

  return true;
}

void useProgrammeNetwork(const BayesianNetwork &network)
{
  installProgrammeNetwork(&network);
}

void useProgrammeNetworkPrior() { installProgrammeNetwork(nullptr); }

bool trainProgrammeNetwork(const std::string &corpusDirectory,
                           const std::string &outPath,
                           const double priorStrength,
                           FitReport &report,
                           std::string &why,
                           const BuildingClass klass)
{
  std::vector<CorpusProgramme> corpus;

  if (!loadProgrammeCorpus(corpusDirectory, corpus, why))
  {
    return false;
  }

  BayesianNetwork fitted;

  if (!fitProgrammeNetwork(corpus, priorStrength, fitted, report, why, klass))
  {
    return false;
  }

  std::ofstream stream(outPath, std::ios::binary);

  if (!stream)
  {
    why = "cannot write " + outPath;

    return false;
  }

  stream << fitted.toJson() << "\n";

  if (!stream)
  {
    why = "failed while writing " + outPath;

    return false;
  }

  return true;
}

bool trainProgrammeNetworks(const std::string &corpusDirectory,
                            const std::string &directory,
                            const double priorStrength,
                            const uint32_t minimumRecords,
                            std::vector<std::pair<BuildingClass, FitReport>> &trained,
                            std::string &why)
{
  trained.clear();

  std::vector<CorpusProgramme> corpus;

  if (!loadProgrammeCorpus(corpusDirectory, corpus, why))
  {
    return false;
  }

  std::map<BuildingClass, uint32_t> counts;

  for (const CorpusProgramme &programme : corpus)
  {
    ++counts[buildingClassOfKind(programme.kind)];
  }

  for (uint32_t k = 0; k < uint32_t(BuildingClass::Count); ++k)
  {
    const BuildingClass klass = BuildingClass(k);

    // A network fitted to one example is that example with rounding on it.
    // Below the threshold the hand-authored prior stands and no file is
    // written, so the generator finds none and uses the prior.
    if (counts[klass] < minimumRecords)
    {
      continue;
    }

    const std::string path = directory + "/programme-network-" + name(klass) + ".json";

    FitReport report;

    if (!trainProgrammeNetwork(corpusDirectory, path, priorStrength, report, why, klass))
    {
      return false;
    }

    trained.emplace_back(klass, report);
  }

  return true;
}

bool loadProgrammeNetwork(const std::string &path, std::string &why)
{
  std::ifstream stream(path, std::ios::binary);

  if (!stream)
  {
    why = "no network at " + path;

    return false;
  }

  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());

  BayesianNetwork network;

  if (!network.fromJson(text, why))
  {
    why = path + ": " + why;

    return false;
  }

  useProgrammeNetwork(network);

  return true;
}

void useProgrammeNetwork(const BayesianNetwork &network, const BuildingClass klass)
{
  installProgrammeNetwork(&network, klass);
}

}  // namespace procedural::world::generator
