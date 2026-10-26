#include "procedural/world/generator/ProgrammeCorpus.hpp"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace procedural::world::generator {

namespace {

constexpr double sqFtToSqM = 0.09290304;

// The corpus vocabulary against the generator's. A word absent from here is
// not an error: the book names rooms this engine has no slot for, and which
// of them earn one is decided by counting plates, not in advance. See the
// corpus report.
RoomUse mapUse(const std::string &use)
{
  static const std::vector<std::pair<const char *, RoomUse>> table = {
    {"kitchen", RoomUse::Kitchen},
    {"scullery", RoomUse::Scullery},
    {"larder", RoomUse::Larder},
    {"pantry", RoomUse::Pantry},
    {"dairy", RoomUse::Dairy},
    {"washhouse", RoomUse::Washhouse},
    {"cellar", RoomUse::Cellar},
    {"parlour", RoomUse::Parlour},
    {"living_room", RoomUse::LivingRoom},
    {"library", RoomUse::Library},
    {"dining_room", RoomUse::DiningRoom},
    {"drawing_room", RoomUse::DrawingRoom},
    {"breakfast_room", RoomUse::BreakfastRoom},
    {"study", RoomUse::Study},
    {"hall", RoomUse::Hall},
    {"passage", RoomUse::Passage},
    {"landing", RoomUse::Landing},
    {"staircase", RoomUse::Staircase},
    {"lobby", RoomUse::Lobby},
    {"porch", RoomUse::Porch},
    {"bedroom", RoomUse::Bedroom},
    {"chamber", RoomUse::Bedroom},
    {"dressing_room", RoomUse::DressingRoom},
    {"nursery", RoomUse::Nursery},
    {"closet", RoomUse::Closet},
    {"wc", RoomUse::WaterCloset},
    {"bathroom", RoomUse::Bathroom},
    {"office", RoomUse::Office},
    {"shop", RoomUse::Shop},
    {"workshop", RoomUse::Workshop},
    {"store", RoomUse::Store},
    {"yard", RoomUse::Yard},

    // An area is the sunken well that lights a basement from the street. It is
    // outside, like a yard, and like a yard it is not floor area.
    {"area", RoomUse::Yard},
    {"court", RoomUse::Yard},

    // A butler's room is a service room off the basement passage; the engine
    // has no better slot and an office is what it is used as.
    {"butlers_room", RoomUse::Office},
  };

  for (const auto &entry : table)
  {
    if (use == entry.first)
    {
      return entry.second;
    }
  }

  return RoomUse::Count;
}

}  // namespace

RoomUse roomUseFromCorpusName(const std::string &use) { return mapUse(use); }

bool CorpusMassing::has(const std::string &kind) const
{
  for (const CorpusFeature &feature : features)
  {
    if (feature.kind == kind)
    {
      return true;
    }
  }

  return false;
}

uint32_t CorpusProgramme::countOf(const std::string &use) const
{
  uint32_t n = 0;

  for (const CorpusRoom &room : rooms)
  {
    n += room.use == use ? 1 : 0;
  }

  return n;
}

bool CorpusProgramme::drewFloor(const int32_t level) const
{
  return std::find(floorsDrawn.begin(), floorsDrawn.end(), level) != floorsDrawn.end();
}

bool loadProgrammeCorpus(const std::string &directory,
                         std::vector<CorpusProgramme> &out,
                         std::string &why)
{
  namespace fs = std::filesystem;

  out.clear();
  why.clear();

  std::error_code error;

  if (!fs::is_directory(directory, error))
  {
    why = "no corpus directory at " + directory;

    return false;
  }

  std::vector<fs::path> files;

  for (const fs::directory_entry &entry : fs::directory_iterator(directory, error))
  {
    const std::string name = entry.path().filename().string();

    if (name.rfind("plate-", 0) == 0 && entry.path().extension() == ".json")
    {
      files.push_back(entry.path());
    }
  }

  // Directory order is not an order. Sort, so the corpus is the same sequence
  // on every machine and a fit is reproducible.
  std::sort(files.begin(), files.end());

  for (const fs::path &file : files)
  {
    std::ifstream stream(file);

    if (!stream)
    {
      why = "cannot read " + file.string();

      return false;
    }

    nlohmann::json document;

    try
    {
      stream >> document;
    }
    catch (const std::exception &e)
    {
      why = file.filename().string() + ": " + e.what();

      return false;
    }

    CorpusProgramme programme;
    programme.id = file.stem().string();
    programme.page = document.value("page", 0u);
    programme.kind = document.value("kind", std::string());
    programme.source = document.value("source", std::string());
    programme.notes = document.value("notes", std::string());
    programme.storeys = document.value("storeys", 1u);
    programme.basement = document.value("basement", false);
    programme.dwelling = document.value("dwelling", true);
    programme.sameHouseAs = document.value("sameHouseAs", 0u);

    for (const auto &level : document.value("floorsDrawn", nlohmann::json::array()))
    {
      programme.floorsDrawn.push_back(level.get<int32_t>());
    }

    const nlohmann::json confidence = document.value("confidence", nlohmann::json::object());
    programme.roomConfidence = confidence.value("rooms", std::string("unknown"));
    programme.dimensionConfidence = confidence.value("dimensions", std::string("unknown"));
    programme.adjacencyConfidence = confidence.value("adjacency", std::string("unknown"));
    programme.adjacencySource = document.value("adjacencySource", std::string("inferred"));

    const nlohmann::json mass = document.value("massing", nlohmann::json::object());
    programme.massing.attachment = mass.value("attachment", std::string());
    programme.massing.plan = mass.value("plan", std::string());
    programme.massing.roof = mass.value("roof", std::string());
    programme.massing.frontageFeet = mass.value("frontageFeet", 0.);
    programme.massing.depthFeet = mass.value("depthFeet", 0.);
    programme.massing.plotFrontageFeet = mass.value("plotFrontageFeet", 0.);
    programme.massing.storeysInRoof = mass.value("storeysInRoof", 0u);
    programme.massing.confidence = mass.value("confidence", std::string("unknown"));

    for (const auto &feature : mass.value("features", nlohmann::json::array()))
    {
      CorpusFeature entry;
      entry.kind = feature.value("kind", std::string());
      entry.count = feature.value("count", 1u);

      if (feature.contains("feet") && feature["feet"].is_array() && feature["feet"].size() == 2)
      {
        const double first = feature["feet"][0].get<double>();
        const double second = feature["feet"][1].get<double>();

        if (first > 0 && second > 0)
        {
          entry.dimensioned = true;
          entry.feetLong = std::max(first, second);
          entry.feetShort = std::min(first, second);
        }
      }

      if (entry.kind.empty())
      {
        why = programme.id + ": a massing feature with no kind";

        return false;
      }

      programme.massing.features.push_back(std::move(entry));
    }

    // Rooms first, all floors, so an adjacency may cross one - the farm house
    // reaches its offices from the kitchen by way of a sixty-foot passage.
    struct PendingLink
    {
      std::string a, b;
      bool door;
    };

    std::vector<PendingLink> pending;

    for (const auto &floor : document.value("floors", nlohmann::json::array()))
    {
      const int32_t level = floor.value("level", 0);

      for (const auto &room : floor.value("rooms", nlohmann::json::array()))
      {
        CorpusRoom entry;
        entry.use = room.value("use", std::string());
        entry.qualifier = room.value("qualifier", std::string());
        entry.raw = room.value("raw", std::string());
        entry.floor = level;
        entry.mapped = mapUse(entry.use);

        if (entry.use.empty())
        {
          why = programme.id + ": a room with no use";

          return false;
        }

        if (room.contains("feet") && room["feet"].is_array() && room["feet"].size() == 2)
        {
          const double first = room["feet"][0].get<double>();
          const double second = room["feet"][1].get<double>();

          if (first > 0 && second > 0)
          {
            entry.dimensioned = true;
            entry.feetLong = std::max(first, second);
            entry.feetShort = std::min(first, second);
            entry.areaMetres = entry.feetLong * entry.feetShort * sqFtToSqM;
            entry.aspect = entry.feetLong / entry.feetShort;
          }
        }

        programme.rooms.push_back(std::move(entry));
      }

      for (const auto &link : floor.value("adjacencies", nlohmann::json::array()))
      {
        pending.push_back({link.value("a", std::string()),
                           link.value("b", std::string()),
                           link.value("link", std::string("door")) != "open"});
      }
    }

    // An adjacency names rooms by key - "cellar:beer" - or by use alone when
    // the plate has only one of them.
    auto resolve = [&programme](const std::string &name) -> uint32_t {
      for (uint32_t i = 0; i < programme.rooms.size(); ++i)
      {
        if (programme.rooms[i].key() == name)
        {
          return i;
        }
      }

      for (uint32_t i = 0; i < programme.rooms.size(); ++i)
      {
        if (programme.rooms[i].use == name)
        {
          return i;
        }
      }

      return uint32_t(programme.rooms.size());
    };

    for (const PendingLink &link : pending)
    {
      const uint32_t a = resolve(link.a);
      const uint32_t b = resolve(link.b);

      if (a >= programme.rooms.size() || b >= programme.rooms.size())
      {
        // A typo here is a silent corruption of the training data, so it is
        // an error. The transcription is hand-written; this is what catches it.
        why = programme.id + ": adjacency names a room that is not there ('"
              + link.a + "' to '" + link.b + "')";

        return false;
      }

      if (a == b)
      {
        why = programme.id + ": adjacency from '" + link.a + "' to itself";

        return false;
      }

      programme.links.push_back({a, b, link.door});
    }

    if (programme.rooms.empty())
    {
      why = programme.id + ": no rooms";

      return false;
    }

    // Two rooms with the same key cannot both be named by an adjacency, so the
    // second is unreachable by construction and the first silently collects
    // every door meant for either. Four bedrooms with one qualifier between
    // them is the shape this takes, and it is a corruption rather than an
    // inconvenience: qualify them.
    for (uint32_t i = 0; i < programme.rooms.size(); ++i)
    {
      for (uint32_t j = i + 1; j < programme.rooms.size(); ++j)
      {
        if (programme.rooms[i].key() == programme.rooms[j].key())
        {
          why = programme.id + ": two rooms share the key '" + programme.rooms[i].key()
                + "'; an adjacency cannot tell them apart";

          return false;
        }
      }
    }

    out.push_back(std::move(programme));
  }

  if (out.empty())
  {
    why = "no plate-*.json in " + directory;

    return false;
  }

  return true;
}

}  // namespace procedural::world::generator
