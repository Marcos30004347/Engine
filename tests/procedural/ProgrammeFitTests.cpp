// Fitting the programme network to the book, and whether it helped.
//
// Stage B1 of .plan/bayesian-architectural-program.md. The structure is the
// hand-authored one; only the numbers move, by maximum a posteriori estimation
// with the hand-authored CPTs as the prior.
//
// The question this has to answer is not "did the fit run" but "does the
// generator now produce the book's houses". So the last and longest check
// samples from the fitted network and puts its room frequencies beside the
// corpus's, standing by standing. A fit that improves the likelihood and
// leaves the sampled houses unchanged has done nothing.
#include "procedural/world/generator/ProgrammeCorpus.hpp"
#include "procedural/world/generator/ProgrammeFit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace procedural;
using namespace procedural::learn;
using namespace procedural::world::generator;

namespace {

int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";

    return;
  }

  std::cerr << "  FAILED " << what << "\n";
  ++failures;
}

// Share of sampled houses of a standing that carry each room.
std::map<std::string, double> sampledFrequency(const PlotStanding standing,
                                               const uint32_t houses)
{
  std::map<std::string, uint32_t> counts;
  uint32_t drawn = 0;

  for (uint32_t i = 0; i < houses; ++i)
  {
    ProgrammeBrief brief;
    brief.standing = standing;

    Programme programme;

    if (!sampleProgramme(brief, 0x51F7u + i * 2654435761u, programme))
    {
      continue;
    }

    ++drawn;
    std::set<std::string> seen;

    for (const RoomSpec &room : programme.rooms)
    {
      seen.insert(name(room.use));
    }

    for (const std::string &use : seen)
    {
      ++counts[use];
    }
  }

  std::map<std::string, double> shares;

  for (const auto &entry : counts)
  {
    shares[entry.first] = double(entry.second) / double(std::max(drawn, 1u));
  }

  return shares;
}

// Share of corpus records of a standing that carry each room. Only records
// that described the whole house count, because a basement plan cannot say a
// house has no parlour.
std::map<std::string, double> corpusFrequency(const std::vector<CorpusProgramme> &corpus,
                                              const PlotStanding standing,
                                              uint32_t &records)
{
  std::map<std::string, uint32_t> counts;
  records = 0;

  for (const CorpusProgramme &programme : corpus)
  {
    PlotStanding its = PlotStanding::Modest;

    if (!programme.dwelling || !standingOfKind(programme.kind, its) || its != standing)
    {
      continue;
    }

    if (programme.floorsDrawn.size() < 2)
    {
      continue;
    }

    ++records;
    std::set<std::string> seen;

    for (const CorpusRoom &room : programme.rooms)
    {
      if (room.mapped != RoomUse::Count)
      {
        seen.insert(name(room.mapped));
      }
    }

    for (const std::string &use : seen)
    {
      ++counts[use];
    }
  }

  std::map<std::string, double> shares;

  for (const auto &entry : counts)
  {
    shares[entry.first] = double(entry.second) / double(std::max(records, 1u));
  }

  return shares;
}

// How far the sampled house differs from the corpus, over the rooms the corpus
// has an opinion about: the mean absolute difference in share.
double disagreement(const std::map<std::string, double> &sampled,
                    const std::map<std::string, double> &observed,
                    std::vector<std::pair<double, std::string>> &worst)
{
  double total = 0;
  uint32_t n = 0;
  worst.clear();

  for (const auto &entry : observed)
  {
    const auto it = sampled.find(entry.first);
    const double mine = it == sampled.end() ? 0. : it->second;
    const double gap = std::abs(mine - entry.second);

    worst.emplace_back(gap, entry.first);
    total += gap;
    ++n;
  }

  std::sort(worst.rbegin(), worst.rend());

  return n == 0 ? 0. : total / double(n);
}

const char *names[] = {"slum", "modest", "comfortable", "wealthy", "grand", "rural"};

}  // namespace

int main()
{
  const std::string directory = ARCHITECTURAL_PROGRAMME_CORPUS_DIR;

  std::vector<CorpusProgramme> corpus;
  std::string why;

  std::cout << "loading\n";
  check(loadProgrammeCorpus(directory, corpus, why), "the corpus loads: " + why);

  if (corpus.empty())
  {
    return 1;
  }

  // --- the standing table ---

  std::cout << "\nplacing the book's building types on the standings\n";

  std::map<PlotStanding, std::vector<std::string>> placed;
  std::vector<std::string> unplaced;

  for (const CorpusProgramme &programme : corpus)
  {
    PlotStanding standing = PlotStanding::Modest;

    if (standingOfKind(programme.kind, standing))
    {
      placed[standing].push_back(programme.kind);
    }
    else
    {
      unplaced.push_back(programme.kind + " (" + programme.id + ")");
    }
  }

  for (uint32_t s = 0; s < 6; ++s)
  {
    std::printf("  %-12s %zu\n", names[s], placed[PlotStanding(s)].size());
  }

  if (!unplaced.empty())
  {
    std::printf("  unplaced:");

    for (const std::string &kind : unplaced)
    {
      std::printf(" %s;", kind.c_str());
    }

    std::printf("\n");
  }

  check(placed[PlotStanding::Grand].size() > 0 && placed[PlotStanding::Modest].size() > 0
          && placed[PlotStanding::Comfortable].size() > 0,
        "the corpus spreads across the standings");

  // --- the fit ---

  std::cout << "\nfitting\n";

  BayesianNetwork fitted;
  FitReport report;

  check(fitProgrammeNetwork(corpus, 4., fitted, report, why), "the fit runs: " + why);

  std::printf("  %u programmes, %u observations over %u of %u nodes\n",
              report.programmes, report.totalObservations, report.nodesWithEvidence,
              fitted.variableCount());
  std::printf("  mean log-likelihood of a record: %.2f under the prior, %.2f fitted\n",
              report.priorLogLikelihood, report.fittedLogLikelihood);

  check(report.programmes > 40, "most of the corpus was placed and counted");
  check(report.fittedLogLikelihood > report.priorLogLikelihood,
        "the corpus is more likely under the fitted network than under the prior");

  // Which nodes the corpus actually has something to say about. A node with a
  // handful of observations is still reporting the prior back at you.
  std::vector<std::pair<uint32_t, std::string>> byEvidence;

  for (uint32_t v = 0; v < fitted.variableCount(); ++v)
  {
    byEvidence.emplace_back(report.observations[v], fitted.variableAt(v).name);
  }

  std::sort(byEvidence.rbegin(), byEvidence.rend());

  std::printf("  best observed nodes:");

  for (size_t i = 0; i < byEvidence.size() && i < 8; ++i)
  {
    std::printf(" %s %u;", byEvidence[i].second.c_str(), byEvidence[i].first);
  }

  std::printf("\n  nodes the corpus never speaks to: %u\n",
              fitted.variableCount() - report.nodesWithEvidence);

  // --- does it still make houses ---

  std::cout << "\nsampling from the fitted network\n";

  useProgrammeNetwork(fitted);

  uint32_t noKitchen = 0, stranded = 0, sampled = 0, rooms = 0;

  for (uint32_t s = 0; s < 6; ++s)
  {
    for (uint32_t i = 0; i < 200; ++i)
    {
      ProgrammeBrief brief;
      brief.standing = PlotStanding(s);

      Programme programme;

      if (!sampleProgramme(brief, 0xF17u + i + s * 1000u, programme))
      {
        continue;
      }

      ++sampled;
      if (!programme.has(RoomUse::Kitchen) && !programme.has(RoomUse::Scullery)
          && !programme.has(RoomUse::LivingRoom))
      {
        if (noKitchen == 0)
        {
          std::printf("  nowhere to cook: %s, seed %u, rooms", names[s], 0xF17u + i + s * 1000u);

          for (const RoomSpec &room : programme.rooms)
          {
            std::printf(" %s", name(room.use));
          }

          std::printf("\n");
        }

        ++noKitchen;
      }
      stranded += programme.repairedLinks;
      rooms += uint32_t(programme.rooms.size());
    }
  }

  check(sampled == 1200, "the fitted network samples every standing");
  // Not a kitchen: somewhere to cook. The book's cottages have a living room
  // and a scullery and no room called a kitchen, and the fit learned that.
  check(noKitchen == 0, "no house without somewhere to cook, still");
  std::printf("  doors the decoder had to add, per room: %.3f\n",
              double(stranded) / double(std::max(rooms, 1u)));

  // --- the number that matters ---

  std::cout << "\nsampled rooms against the book's, by standing\n";

  double before = 0, after = 0;
  uint32_t compared = 0;

  for (uint32_t s = 0; s < 6; ++s)
  {
    const PlotStanding standing = PlotStanding(s);

    uint32_t records = 0;
    const std::map<std::string, double> observed = corpusFrequency(corpus, standing, records);

    if (records < 3)
    {
      continue;
    }

    useProgrammeNetworkPrior();
    std::vector<std::pair<double, std::string>> worstPrior;
    const double gapPrior =
      disagreement(sampledFrequency(standing, 400), observed, worstPrior);

    useProgrammeNetwork(fitted);
    std::vector<std::pair<double, std::string>> worstFitted;
    const double gapFitted =
      disagreement(sampledFrequency(standing, 400), observed, worstFitted);

    std::printf("  %-12s %u records: mean gap %.3f prior, %.3f fitted; worst now",
                names[s], records, gapPrior, gapFitted);

    for (size_t i = 0; i < worstFitted.size() && i < 3; ++i)
    {
      std::printf(" %s %.2f;", worstFitted[i].second.c_str(), worstFitted[i].first);
    }

    std::printf("\n");

    before += gapPrior;
    after += gapFitted;
    ++compared;
  }

  if (compared > 0)
  {
    std::printf("  over %u standings: %.3f prior, %.3f fitted\n",
                compared, before / compared, after / compared);
  }

  check(compared >= 3, "at least three standings have enough records to compare");
  check(after < before,
        "the fitted network's houses look more like the book's than the prior's do");

  // --- write the trained network out ---
  //
  // The generator loads this file on first use. Training here rather than in a
  // separate tool means the asset cannot drift from the corpus without a test
  // failing, which is the only way a generated asset stays honest.
  std::cout << "\ntraining\n";

  const std::string assets = ARCHITECTURAL_PROGRAMME_CORPUS_DIR;

  // How the corpus divides between the classes, and therefore which of them
  // can be fitted at all.
  std::map<std::string, uint32_t> perClass;

  for (const CorpusProgramme &programme : corpus)
  {
    ++perClass[name(buildingClassOfKind(programme.kind))];
  }

  std::printf("  corpus by building class:");

  for (const auto &entry : perClass)
  {
    std::printf(" %s %u;", entry.first.c_str(), entry.second);
  }

  std::printf("\n");

  std::vector<std::pair<BuildingClass, FitReport>> trained;
  check(trainProgrammeNetworks(directory, assets, 4., 8, trained, why),
        "training every class the corpus can carry: " + why);

  for (const auto &entry : trained)
  {
    std::printf("  fitted %-10s %u programmes, %u observations, %.2f -> %.2f\n",
                name(entry.first), entry.second.programmes,
                entry.second.totalObservations,
                entry.second.priorLogLikelihood, entry.second.fittedLogLikelihood);
  }

  check(!trained.empty(), "at least one class had enough records to fit");

  const std::string outPath = assets + "/programme-network-dwelling.json";

  BayesianNetwork reloaded;
  std::string reloadWhy;
  check(loadProgrammeNetwork(outPath, reloadWhy),
        "the trained dwelling network loads back: " + reloadWhy);

  // A round trip has to be exact enough that the houses do not change. Six
  // figures per cell is the format's precision, so compare against that.
  useProgrammeNetworkPrior();

  BayesianNetwork check1;
  FitReport ignored;
  std::string ignoredWhy;
  fitProgrammeNetwork(corpus, 4., check1, ignored, ignoredWhy);

  std::ifstream stream(outPath, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());

  BayesianNetwork roundTripped;
  std::string parseWhy;
  check(roundTripped.fromJson(text, parseWhy), "the file parses: " + parseWhy);
  check(roundTripped.variableCount() == check1.variableCount(),
        "the round trip keeps every variable");

  double worst = 0;

  for (uint32_t v = 0; v < check1.variableCount() && v < roundTripped.variableCount(); ++v)
  {
    for (uint32_t r = 0; r < check1.rowCount(v); ++r)
    {
      const std::vector<double> a = check1.row(v, r);
      const std::vector<double> b = roundTripped.row(v, r);

      for (size_t i = 0; i < a.size() && i < b.size(); ++i)
      {
        worst = std::max(worst, std::abs(a[i] - b[i]));
      }
    }
  }

  std::printf("  worst cell difference across the round trip: %.2e\n", worst);
  check(worst < 1e-5, "the round trip is exact to the format's precision");

  useProgrammeNetworkPrior();

  // --- one network per building class ---

  std::cout << "\nthe building classes\n";

  for (uint32_t k = 0; k < uint32_t(BuildingClass::Count); ++k)
  {
    const BuildingClass klass = BuildingClass(k);
    const BayesianNetwork &net = priorProgrammeNetwork(klass);

    uint32_t edges = 0;

    for (uint32_t v = 0; v < net.variableCount(); ++v)
    {
      edges += uint32_t(net.parentsOf(v).size());
    }

    std::string invalid;
    const bool valid = net.validate(invalid);

    std::printf("  %-10s %3zu rooms, %3u nodes, %3u edges%s\n", name(klass),
                programmeVocabulary(klass).size(), net.variableCount(), edges,
                valid ? "" : (" INVALID: " + invalid).c_str());

    check(valid, std::string("the ") + name(klass) + " network validates");
  }

  // Each class samples, and samples its own rooms and not another's.
  uint32_t sampledClasses = 0;

  for (uint32_t k = 0; k < uint32_t(BuildingClass::Count); ++k)
  {
    const BuildingClass klass = BuildingClass(k);

    ProgrammeBrief brief;
    brief.building = klass;
    brief.standing = PlotStanding::Comfortable;

    Programme programme;

    if (!sampleProgramme(brief, 0xC1A55u + k, programme))
    {
      continue;
    }

    ++sampledClasses;

    std::set<std::string> rooms;

    for (const RoomSpec &room : programme.rooms)
    {
      rooms.insert(name(room.use));
    }

    std::string joined;

    for (const std::string &room : rooms)
    {
      joined += (joined.empty() ? "" : " ") + room;
    }

    std::printf("    %-10s %s\n", name(klass), joined.c_str());

    // The vocabulary is the contract: a class must not produce a room its
    // network has no node for.
    const std::vector<RoomUse> &vocabulary = programmeVocabulary(klass);
    bool inVocabulary = true;

    for (const RoomSpec &room : programme.rooms)
    {
      inVocabulary = inVocabulary
                     && std::find(vocabulary.begin(), vocabulary.end(), room.use)
                          != vocabulary.end();
    }

    check(inVocabulary,
          std::string("the ") + name(klass) + " draws only its own rooms");
  }

  check(sampledClasses == uint32_t(BuildingClass::Count), "every class samples");

  // The classes have to be different buildings, not one building relabelled.
  ProgrammeBrief innBrief;
  innBrief.building = BuildingClass::Inn;
  innBrief.standing = PlotStanding::Comfortable;

  ProgrammeBrief houseBrief;
  houseBrief.building = BuildingClass::Dwelling;
  houseBrief.standing = PlotStanding::Comfortable;

  uint32_t innsWithBar = 0, housesWithBar = 0, warehousesWithBed = 0;

  for (uint32_t i = 0; i < 200; ++i)
  {
    Programme inn, house, store;

    if (sampleProgramme(innBrief, 0x1AA0u + i, inn))
    {
      innsWithBar += inn.has(RoomUse::Bar) ? 1 : 0;
    }

    if (sampleProgramme(houseBrief, 0x40u + i, house))
    {
      housesWithBar += house.has(RoomUse::Bar) ? 1 : 0;
    }

    ProgrammeBrief warehouseBrief;
    warehouseBrief.building = BuildingClass::Warehouse;
    warehouseBrief.standing = PlotStanding::Comfortable;

    if (sampleProgramme(warehouseBrief, 0x5100u + i, store))
    {
      warehousesWithBed += store.has(RoomUse::Bedroom) ? 1 : 0;
    }
  }

  std::printf("  inns with a bar %u/200, houses with a bar %u/200, "
              "warehouses with a bedroom %u/200\n",
              innsWithBar, housesWithBar, warehousesWithBed);

  check(innsWithBar > 180, "an inn has a bar");
  check(housesWithBar == 0, "a house has no bar");
  check(warehousesWithBed == 0, "a warehouse has no bedroom");

  // Each class has a room that makes it that class. If the network can leave
  // it out, the class is not doing its job: a church with no nave is a hall.
  struct Signature
  {
    BuildingClass klass;
    PlotStanding standing;
    RoomUse must;
    const char *what;
  };

  const Signature signatures[] = {
    {BuildingClass::Church, PlotStanding::Comfortable, RoomUse::Nave, "a church has a nave"},
    {BuildingClass::Chapel, PlotStanding::Modest, RoomUse::Nave, "a chapel has a nave"},
    {BuildingClass::Temple, PlotStanding::Comfortable, RoomUse::Sanctuary,
     "a temple has a sanctuary"},
    {BuildingClass::Prison, PlotStanding::Comfortable, RoomUse::Cell, "a prison has cells"},
    {BuildingClass::ManorHouse, PlotStanding::Wealthy, RoomUse::GreatHall,
     "a manor house has a great hall"},
    {BuildingClass::Castle, PlotStanding::Grand, RoomUse::Courtyard,
     "a castle has a courtyard"},
    {BuildingClass::Mansion, PlotStanding::Grand, RoomUse::Saloon,
     "a grand mansion has a saloon"},
  };

  for (const Signature &signature : signatures)
  {
    ProgrammeBrief brief;
    brief.building = signature.klass;
    brief.standing = signature.standing;

    uint32_t with = 0, drawn = 0;

    for (uint32_t i = 0; i < 150; ++i)
    {
      Programme programme;

      if (!sampleProgramme(brief, 0x5164u + i * 7919u, programme))
      {
        continue;
      }

      ++drawn;
      with += programme.has(signature.must) ? 1 : 0;
    }

    std::printf("  %-12s %s in %u of %u\n", name(signature.klass),
                name(signature.must), with, drawn);
    check(drawn > 0 && with * 10 >= drawn * 7, signature.what);
  }

  // And the rooms that belong to one class must not leak into another.
  const std::pair<BuildingClass, RoomUse> forbidden[] = {
    {BuildingClass::Dwelling, RoomUse::Nave},
    {BuildingClass::Dwelling, RoomUse::Cell},
    {BuildingClass::Church, RoomUse::Bedroom},
    {BuildingClass::Church, RoomUse::Kitchen},
    {BuildingClass::Castle, RoomUse::DrawingRoom},
    {BuildingClass::Prison, RoomUse::Ballroom},
    {BuildingClass::Warehouse, RoomUse::Nave},
  };

  for (const auto &entry : forbidden)
  {
    uint32_t leaked = 0;

    for (uint32_t i = 0; i < 120; ++i)
    {
      ProgrammeBrief brief;
      brief.building = entry.first;
      brief.standing = PlotStanding::Grand;

      Programme programme;

      if (sampleProgramme(brief, 0x1EA4u + i, programme))
      {
        leaked += programme.has(entry.second) ? 1 : 0;
      }
    }

    check(leaked == 0, std::string("a ") + name(entry.first) + " has no "
                         + name(entry.second));
  }

  std::cout << "\n" << (failures == 0 ? "all passed" : std::to_string(failures) + " failed")
            << "\n";

  return failures == 0 ? 0 : 1;
}
