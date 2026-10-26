// The transcribed corpus, and whether it is still the book.
//
// Stage A of .plan/bayesian-architectural-program.md. The training data is
// hand-read off 784 page scans, which means every kind of error a person
// makes: a room named in an adjacency that was never listed, a dimension
// typed with the feet and inches swapped, a plate transcribed twice. None of
// those announce themselves in a fit - they come out as a network that
// generates slightly wrong houses - so they are caught here instead.
//
// The report this prints is the acceptance the plan asks for: room frequency
// and co-occurrence, to be checked against the counts taken off the OCR
// before any of this was transcribed. If the two drift apart, the
// transcription is drifting from the book.
#include "procedural/world/generator/ProgrammeCorpus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

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

// Rooms of a programme that no adjacency reaches from its entrance.
//
// A transcription is a reading, not a design, so a stranded room here is
// usually a plate whose doors were not legible rather than a house with a
// sealed larder - which is why this is reported per room and gated loosely.
// The yard and the area are outside. A plate draws them and labels them, and
// often draws no door to them at all, because the door is round the back.
bool outdoors(const std::string &use)
{
  return use == "yard" || use == "area" || use == "court";
}

std::vector<std::string> unreachableRooms(const CorpusProgramme &programme)
{
  std::vector<std::vector<uint32_t>> neighbours(programme.rooms.size());

  for (const CorpusLink &link : programme.links)
  {
    neighbours[link.a].push_back(link.b);
    neighbours[link.b].push_back(link.a);
  }

  uint32_t start = 0;

  for (uint32_t i = 0; i < programme.rooms.size(); ++i)
  {
    const std::string &use = programme.rooms[i].use;

    if (use == "porch" || use == "lobby" || use == "hall" || use == "passage")
    {
      start = i;

      break;
    }
  }

  // Seed from the entrance *and* from every outdoor space, because you can
  // always walk round the outside of a house: the coal cellar is reached down
  // a shoot from the area, and the vault opens off it.
  std::vector<bool> seen(programme.rooms.size(), false);
  std::vector<uint32_t> stack = {start};
  seen[start] = true;

  for (uint32_t i = 0; i < programme.rooms.size(); ++i)
  {
    if (outdoors(programme.rooms[i].use) && !seen[i])
    {
      seen[i] = true;
      stack.push_back(i);
    }
  }

  while (!stack.empty())
  {
    const uint32_t r = stack.back();
    stack.pop_back();

    for (const uint32_t n : neighbours[r])
    {
      if (!seen[n])
      {
        seen[n] = true;
        stack.push_back(n);
      }
    }
  }

  std::vector<std::string> lost;

  for (uint32_t i = 0; i < seen.size(); ++i)
  {
    if (!seen[i] && !outdoors(programme.rooms[i].use))
    {
      lost.push_back(programme.rooms[i].key());
    }
  }

  return lost;
}

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

  // --- what is in it ---

  uint32_t rooms = 0, dimensioned = 0, links = 0, doors = 0, stranded = 0;
  uint32_t withThreeDimensioned = 0, multiFloor = 0;
  std::map<std::string, uint32_t> frequency, kinds, unmapped;
  std::map<std::string, uint32_t> pairs;
  std::set<uint32_t> pages;

  for (const CorpusProgramme &programme : corpus)
  {
    rooms += uint32_t(programme.rooms.size());
    links += uint32_t(programme.links.size());
    pages.insert(programme.page);
    ++kinds[programme.kind];
    const std::vector<std::string> lost = unreachableRooms(programme);
    stranded += uint32_t(lost.size());

    if (!lost.empty())
    {
      std::string joined;

      for (const std::string &room : lost)
      {
        joined += (joined.empty() ? "" : ", ") + room;
      }

      std::printf("  %-14s no door reaches: %s\n", programme.id.c_str(), joined.c_str());
    }
    multiFloor += programme.floorsDrawn.size() > 1 ? 1 : 0;

    uint32_t here = 0;
    std::set<std::string> seen;

    for (const CorpusRoom &room : programme.rooms)
    {
      dimensioned += room.dimensioned ? 1 : 0;
      here += room.dimensioned ? 1 : 0;
      seen.insert(room.use);

      if (room.mapped == RoomUse::Count)
      {
        ++unmapped[room.use];
      }
    }

    withThreeDimensioned += here >= 3 ? 1 : 0;

    for (const std::string &use : seen)
    {
      ++frequency[use];
    }

    for (const CorpusLink &link : programme.links)
    {
      doors += link.door ? 1 : 0;

      std::string a = programme.rooms[link.a].use;
      std::string b = programme.rooms[link.b].use;

      if (b < a)
      {
        std::swap(a, b);
      }

      ++pairs[a + " + " + b];
    }
  }

  uint32_t dwellings = 0;

  for (const CorpusProgramme &programme : corpus)
  {
    dwellings += programme.dwelling ? 1 : 0;
  }

  std::printf("  %u of them are dwellings; the rest are buildings a town has "
              "in it\n", dwellings);

  std::printf("\n  %zu programmes over %zu pages: %u rooms (%u dimensioned, %.0f%%), "
              "%u adjacencies (%u doors, %u open)\n",
              corpus.size(), pages.size(), rooms, dimensioned,
              100. * double(dimensioned) / double(std::max(rooms, 1u)),
              links, doors, links - doors);
  std::printf("  %u programmes describe more than one floor; %u have three or more "
              "dimensioned rooms\n", multiFloor, withThreeDimensioned);

  std::cout << "\nroom frequency, programmes carrying each\n";

  std::vector<std::pair<uint32_t, std::string>> byCount;

  for (const auto &entry : frequency)
  {
    byCount.emplace_back(entry.second, entry.first);
  }

  std::sort(byCount.rbegin(), byCount.rend());

  for (size_t i = 0; i < byCount.size(); ++i)
  {
    std::printf("  %-16s %2u%s", byCount[i].second.c_str(), byCount[i].first,
                i % 3 == 2 ? "\n" : "   ");
  }

  std::printf("%s\ncо-occurring adjacencies, top 20\n", byCount.size() % 3 ? "\n" : "");

  std::vector<std::pair<uint32_t, std::string>> pairsByCount;

  for (const auto &entry : pairs)
  {
    pairsByCount.emplace_back(entry.second, entry.first);
  }

  std::sort(pairsByCount.rbegin(), pairsByCount.rend());

  for (size_t i = 0; i < pairsByCount.size() && i < 20; ++i)
  {
    std::printf("  %-34s %u\n", pairsByCount[i].second.c_str(), pairsByCount[i].first);
  }

  // --- the massing ---

  std::map<std::string, uint32_t> attachments, plans, roofs, featureKinds;
  uint32_t withFootprint = 0, withPlotFrontage = 0, inTheRoof = 0;

  for (const CorpusProgramme &programme : corpus)
  {
    const CorpusMassing &mass = programme.massing;

    if (!mass.attachment.empty())
    {
      ++attachments[mass.attachment];
    }

    if (!mass.plan.empty())
    {
      ++plans[mass.plan];
    }

    if (!mass.roof.empty())
    {
      ++roofs[mass.roof];
    }

    withFootprint += mass.known() ? 1 : 0;
    withPlotFrontage += mass.plotFrontageFeet > 0 ? 1 : 0;
    inTheRoof += mass.storeysInRoof > 0 ? 1 : 0;

    for (const CorpusFeature &feature : mass.features)
    {
      featureKinds[feature.kind] += feature.count;
    }
  }

  std::printf("\nmassing: %u with a stated footprint, %u with a plot frontage, "
              "%u with storeys in the roof\n", withFootprint, withPlotFrontage, inTheRoof);

  auto histogram = [](const char *label, const std::map<std::string, uint32_t> &counts) {
    std::printf("  %-12s", label);

    for (const auto &entry : counts)
    {
      std::printf(" %s %u;", entry.first.c_str(), entry.second);
    }

    std::printf("\n");
  };

  histogram("attachment", attachments);
  histogram("plan", plans);
  histogram("roof", roofs);
  histogram("features", featureKinds);

  std::map<std::string, uint32_t> adjacencySources;
  uint32_t linksFromEvidence = 0;

  for (const CorpusProgramme &programme : corpus)
  {
    ++adjacencySources[programme.adjacencySource];

    if (programme.adjacencySource != "inferred")
    {
      linksFromEvidence += uint32_t(programme.links.size());
    }
  }

  std::printf("\nhow the adjacencies were got\n");

  for (const auto &entry : adjacencySources)
  {
    std::printf("  %-10s %u programmes\n", entry.first.c_str(), entry.second);
  }

  std::printf("  %u of %u links come from a source that also testifies to the "
              "walls\n", linksFromEvidence, links);

  std::cout << "\nbuilding kinds\n";

  for (const auto &entry : kinds)
  {
    std::printf("  %-52s %u\n", entry.first.c_str(), entry.second);
  }

  // Vocabulary the generator has no slot for. The plan's rule is to prune room
  // types occurring on fewer than about eight plates, so this is the list to
  // read when deciding which of them earn a RoomUse of their own.
  if (!unmapped.empty())
  {
    std::cout << "\nvocabulary with no slot in RoomUse (rooms, not programmes)\n";

    std::vector<std::pair<uint32_t, std::string>> byUse;

    for (const auto &entry : unmapped)
    {
      byUse.emplace_back(entry.second, entry.first);
    }

    std::sort(byUse.rbegin(), byUse.rend());

    for (const auto &entry : byUse)
    {
      std::printf("  %-16s %u\n", entry.second.c_str(), entry.first);
    }
  }

  // --- what must be true of it ---

  std::cout << "\nchecks\n";

  // The floor rises as the transcription goes on. It is here so that deleting
  // half the corpus is a test failure rather than a quiet regression.
  check(corpus.size() >= 19, "at least 19 programmes transcribed ("
                               + std::to_string(corpus.size()) + ")");

  // A programme that describes a whole house must have somewhere to cook. One
  // that describes a single drawn floor need not - the kitchen of a first-rate
  // house is in the basement, and its ground-floor plate is still good data -
  // and neither need one whose basement was never drawn, which is where a
  // second-rate house keeps its kitchen and its offices.
  uint32_t kitchenless = 0;

  for (const CorpusProgramme &programme : corpus)
  {
    const bool serviceFloorUndrawn = programme.basement && !programme.drewFloor(-1);

    if (!programme.dwelling || programme.floorsDrawn.size() < 2 || serviceFloorUndrawn)
    {
      continue;
    }

    kitchenless += programme.countOf("kitchen") + programme.countOf("scullery") == 0 ? 1 : 0;
  }

  check(kitchenless == 0, "every whole-house programme has somewhere to cook ("
                            + std::to_string(kitchenless) + " do not)");

  check(stranded == 0, "every transcribed room is reached by some adjacency ("
                         + std::to_string(stranded) + " are not)");

  // Dimensions. A plate prints feet and inches; anything outside these bounds
  // is a transcription slip - a decimal point in the wrong place, or feet and
  // inches entered as two dimensions.
  bool sane = true;
  std::string worst;

  for (const CorpusProgramme &programme : corpus)
  {
    for (const CorpusRoom &room : programme.rooms)
    {
      if (!room.dimensioned)
      {
        continue;
      }

      // A service passage really is sixty feet by three - the farm house has
      // one - so circulation gets a longer leash than a room does.
      const bool circulation = room.use == "passage" || room.use == "hall"
                               || room.use == "landing";

      if (room.feetShort < 2 || room.feetLong > 80 || room.aspect > (circulation ? 25 : 12))
      {
        sane = false;
        worst = programme.id + " " + room.use + " "
                + std::to_string(room.feetLong) + " by " + std::to_string(room.feetShort);
      }
    }
  }

  check(sane, "every dimension is a room a person fits in" + (worst.empty() ? "" : ": " + worst));

  // Every programme should be a house, which means somewhere to cook and
  // somewhere to sleep - unless the source only drew one floor, in which case
  // absence is absence of evidence.
  uint32_t incomplete = 0;

  for (const CorpusProgramme &programme : corpus)
  {
    const bool wholeHouse = programme.dwelling && programme.floorsDrawn.size() >= 2
                            && !(programme.basement && !programme.drewFloor(-1));
    const bool livable = programme.countOf("kitchen") + programme.countOf("scullery") > 0
                         && programme.countOf("bedroom") > 0;

    incomplete += wholeHouse && !livable ? 1 : 0;
  }

  check(incomplete == 0, "every multi-floor programme has somewhere to cook and to sleep ("
                           + std::to_string(incomplete) + " do not)");

  // The transcription is supposed to reproduce the book, and the book was
  // counted off the OCR before any of this existed: over 182 candidate plates,
  // kitchen led, then hall, closet, bedroom, wc, cellar, scullery. The corpus
  // is a different sample - it leans on the specification pages, which
  // describe whole houses rather than one drawn floor - so the counts will not
  // match, but the order of the top of the table should survive.
  check(frequency["kitchen"] >= frequency["scullery"], "the kitchen still leads the scullery");
  check(frequency["bedroom"] >= frequency["dressing_room"],
        "bedrooms still outnumber dressing rooms");
  check(frequency["staircase"] + frequency["landing"] > 0, "circulation is transcribed");

  // The massing is what lets a programme drive the exterior. Every record has
  // to say at least how the building stands - detached, semi-detached, in a
  // terrace - because that alone changes which walls can have windows and
  // which ends of the roof can be hipped.
  uint32_t withoutAttachment = 0;
  bool footprintSane = true;

  for (const CorpusProgramme &programme : corpus)
  {
    withoutAttachment += programme.massing.attachment.empty() ? 1 : 0;

    const CorpusMassing &mass = programme.massing;

    if (mass.known() && (mass.frontageFeet < 8 || mass.depthFeet < 8
                         || mass.areaSqFt() > 6000))
    {
      footprintSane = false;
    }

    if (mass.plotFrontageFeet > 0 && mass.frontageFeet > 0
        && mass.plotFrontageFeet < mass.frontageFeet)
    {
      footprintSane = false;
    }
  }

  check(withoutAttachment == 0, "every programme says how its building stands ("
                                  + std::to_string(withoutAttachment) + " do not)");
  check(footprintSane, "every stated footprint fits inside its plot and holds a house");

  std::cout << "\n" << (failures == 0 ? "all passed" : std::to_string(failures) + " failed")
            << "\n";

  return failures == 0 ? 0 : 1;
}
