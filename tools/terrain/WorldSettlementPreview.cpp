// Renders the actual-world settlement integration (W0) as SVG/HTML/JSON:
// the world overview with every evaluated site candidate, and the local
// settlement domain with suitability, regions, districts, farmland, exits
// and gates. Vector output is the review evidence; PNGs are rasterised
// from the SVGs by the caller.
#include "json.hpp"
#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapHierarchy.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/WorldSettlementFabricAdapter.hpp"
#include "procedural/world/WorldSettlementSiteAdapter.hpp"
#include "procedural/world/generator/BlockGrammar.hpp"
#include "procedural/world/generator/SettlementGenerators.hpp"
#include "procedural/world/generator/WorldRoads.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
using namespace procedural::world;
using namespace procedural::world::rewrite;
using nlohmann::json;

namespace {
using Poly = std::vector<Position>;

std::string biomeColour(Biome b)
{
  switch (b)
  {
  case Biome::Ocean:
    return "#2f5f9e";
  case Biome::Lake:
    return "#4d8fd1";
  case Biome::Shore:
    return "#d8c89a";
  case Biome::Glacier:
    return "#f0f4f8";
  case Biome::Tundra:
    return "#b9c2b0";
  case Biome::ColdDesert:
    return "#c9c0a8";
  case Biome::SaltFlat:
    return "#e8e4d6";
  case Biome::Taiga:
    return "#3f6b4a";
  case Biome::TemperateForest:
    return "#4f8a45";
  case Biome::TemperateRainforest:
    return "#2f7a4a";
  case Biome::MontaneForest:
    return "#5f7f55";
  case Biome::Mediterranean:
    return "#9aa552";
  case Biome::Grassland:
    return "#a8c66c";
  case Biome::Steppe:
    return "#c6b96a";
  case Biome::Shrubland:
    return "#b6a25e";
  case Biome::Savanna:
    return "#d1b862";
  case Biome::TropicalSeasonalForest:
    return "#5c9a3c";
  case Biome::TropicalRainforest:
    return "#2e7a2e";
  case Biome::SubtropicalForest:
    return "#3f8f4f";
  case Biome::Desert:
    return "#e0c48a";
  case Biome::SandDesert:
    return "#ead9a2";
  case Biome::Wetland:
    return "#7aa87a";
  case Biome::Swamp:
    return "#5f7f5f";
  case Biome::Mangrove:
    return "#4f7f6f";
  case Biome::AlpineMeadow:
    return "#9fc08a";
  case Biome::BareRock:
    return "#8f8a82";
  default:
    return "#888";
  }
}

std::string useColour(DistrictUse u)
{
  switch (u)
  {
  case DistrictUse::Residential:
    return "#e07b5f";
  case DistrictUse::Craftsmen:
    return "#d9a441";
  case DistrictUse::WorkerHousing:
    return "#c56f9a";
  case DistrictUse::Temple:
    return "#8e6bd6";
  case DistrictUse::Civic:
    return "#4a8fd6";
  case DistrictUse::Merchant:
    return "#e2b04a";
  case DistrictUse::Military:
    return "#7a7a7a";
  case DistrictUse::Suburb:
    return "#f0a080";
  case DistrictUse::WealthyResidential:
    return "#f2c14e";
  default:
    return "#999";
  }
}

struct Svg
{
  std::ostringstream out;
  double scale = 1, width = 0, height = 0;

  Svg(double w, double h, double s) : scale(s), width(w), height(h)
  {
    out << std::fixed << std::setprecision(2) << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << " " << h << "\">\n";
  }

  double x(double v) const
  {
    return v * scale;
  }

  double y(double v) const
  {
    return height - v * scale;
  }

  void polygon(const Poly &p, const std::string &fill, const std::string &stroke, double strokeWidth, double opacity = 1)
  {
    if (p.size() < 3)
    {
      return;
    }
    out << "<polygon points=\"";
    for (auto q : p)
    {
      out << x(q.x) << "," << y(q.z) << " ";
    }
    out << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << strokeWidth << "\" fill-opacity=\"" << opacity << "\"/>\n";
  }

  void polyline(const Poly &p, const std::string &stroke, double strokeWidth, const std::string &extra = "")
  {
    if (p.size() < 2)
    {
      return;
    }
    out << "<polyline points=\"";
    for (auto q : p)
    {
      out << x(q.x) << "," << y(q.z) << " ";
    }
    out << "\" fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\"" << strokeWidth << "\" " << extra << "/>\n";
  }

  void rect(double x0, double z0, double w, double h, const std::string &fill, double opacity = 1)
  {
    out << "<rect x=\"" << x(x0) << "\" y=\"" << y(z0 + h) << "\" width=\"" << w * scale << "\" height=\"" << h * scale << "\" fill=\"" << fill << "\" fill-opacity=\"" << opacity << "\"/>\n";
  }

  void circle(Position p, double r, const std::string &fill, const std::string &stroke, double strokeWidth, double opacity = 1)
  {
    out << "<circle cx=\"" << x(p.x) << "\" cy=\"" << y(p.z) << "\" r=\"" << r << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << strokeWidth << "\" fill-opacity=\"" << opacity << "\"/>\n";
  }

  void text(Position p, const std::string &s, double size, const std::string &fill = "#111", const std::string &anchor = "middle")
  {
    out << "<text x=\"" << x(p.x) << "\" y=\"" << y(p.z) << "\" font-size=\"" << size << "\" font-family=\"Helvetica, Arial, sans-serif\" fill=\"" << fill << "\" text-anchor=\"" << anchor << "\">" << s << "</text>\n";
  }

  std::string finish()
  {
    out << "</svg>\n";
    return out.str();
  }
};

double signedAreaOf(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }
  return a * .5;
}

Position centroid(const Poly &p)
{
  Position c;
  for (auto q : p)
  {
    c.x += q.x / p.size();
    c.z += q.z / p.size();
  }
  return c;
}

json poly(const Poly &p)
{
  json a = json::array();
  for (auto v : p)
  {
    a.push_back({v.x, v.y, v.z});
  }
  return a;
}

// Marching squares over a regular grid of heights (cell centres).
std::vector<Poly> contourLines(const std::vector<double> &h, uint32_t cols, uint32_t rows, double x0, double z0, double spacing, double level)
{
  std::vector<Poly> lines;
  auto at = [&](uint32_t i, uint32_t j) {
    return h[j * cols + i];
  };
  auto pos = [&](double i, double j) {
    return Position{x0 + (i + .5) * spacing, 0, z0 + (j + .5) * spacing};
  };
  for (uint32_t j = 0; j + 1 < rows; ++j)
  {
    for (uint32_t i = 0; i + 1 < cols; ++i)
    {
      double v[4] = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
      Position c[4] = {pos(i, j), pos(i + 1, j), pos(i + 1, j + 1), pos(i, j + 1)};
      std::vector<Position> cuts;
      for (int k = 0; k < 4; ++k)
      {
        double a = v[k], b = v[(k + 1) % 4];
        if ((a < level) != (b < level))
        {
          double t = (level - a) / (b - a);
          cuts.push_back({c[k].x + (c[(k + 1) % 4].x - c[k].x) * t, 0, c[k].z + (c[(k + 1) % 4].z - c[k].z) * t});
        }
      }
      if (cuts.size() == 2)
      {
        lines.push_back({cuts[0], cuts[1]});
      }
      else if (cuts.size() == 4)
      {
        lines.push_back({cuts[0], cuts[1]});
        lines.push_back({cuts[2], cuts[3]});
      }
    }
  }
  return lines;
}
} // namespace

int main(int argc, char **argv)
{
  WorldPipelineSettings settings;
  settings.brief.extentMetres = 12000;
  settings.brief.seed = 17;
  settings.brief.reliefMetres = 900;
  settings.brief.tags = {Tag::Coast, Tag::Lowland};
  settings.cellSpacingMetres = 400;
  settings.hierarchyLevels = 1;
  WorldSiteSelectionSettings siteSettings;
  uint32_t population = 120;
  uint32_t kingdomCount = 0; // --kingdom N: compose the first N kingdom settlements for the world sheet.
  uint64_t variant = 0;      // --variant K: jitter the fabric grammar (0 = the class recipe).
  int recipe = -1;           // --recipe organic|planned|lsystem: force the layout recipe.
  int pattern = -1;          // --pattern basic|raster|radial|elevation: road growth pattern.
  bool lotsOnly = false;     // --lots 1: render the Watabou lot allocation without houses.
  bool roads = false;        // --roads 1: the roads-first settlement generator (no wall).
  std::filesystem::path output = "settlement-preview";
  for (int i = 1; i + 1 < argc; i += 2)
  {
    std::string option = argv[i], value = argv[i + 1];
    if (option == "--seed")
    {
      settings.brief.seed = std::stoull(value);
    }
    else if (option == "--extent")
    {
      settings.brief.extentMetres = std::stod(value) * 1000;
    }
    else if (option == "--spacing")
    {
      settings.cellSpacingMetres = std::stod(value) * 1000;
    }
    else if (option == "--relief")
    {
      settings.brief.reliefMetres = std::stod(value);
    }
    else if (option == "--levels")
    {
      settings.hierarchyLevels = uint32_t(std::stoul(value));
    }
    else if (option == "--population")
    {
      population = uint32_t(std::stoul(value));
    }
    else if (option == "--cell")
    {
      siteSettings.preferredCell = uint32_t(std::stoul(value));
    }
    else if (option == "--output")
    {
      output = value;
    }
    else if (option == "--kingdom")
    {
      kingdomCount = uint32_t(std::stoul(value));
    }
    else if (option == "--village-form")
    {
      using Form = WorldSiteSelectionSettings::VillageForm;
      siteSettings.villageForm = value == "cluster"     ? Form::Cluster
                                 : value == "street"    ? Form::Street
                                 : value == "round"     ? Form::Round
                                 : value == "anger"     ? Form::Anger
                                 : value == "scattered" ? Form::Scattered
                                                        : Form::Auto;
    }
    else if (option == "--lots")
    {
      lotsOnly = value == "1" || value == "true" || value == "yes";
    }
    else if (option == "--roads")
    {
      roads = value == "1" || value == "true" || value == "yes";
    }
    else if (option == "--pattern")
    {
      pattern = value == "raster" ? 1 : value == "radial" ? 2 : value == "elevation" ? 3 : value == "basic" ? 0 : -1;
    }
    else if (option == "--recipe")
    {
      recipe = value == "lsystem" ? 3 : value == "planned" ? 2 : value == "organic" ? 0 : -1;
    }
    else if (option == "--variant")
    {
      // Variant K: jittered class grammar plus a plaza of 6/4/8 sides.
      variant = std::stoull(value);
      siteSettings.plazaSides = variant ? std::array<uint32_t, 3>{6, 4, 8}[variant % 3] : 6;
    }
    else if (option == "--tags")
    {
      settings.brief.tags.clear();
      std::stringstream stream(value);
      std::string name;
      while (std::getline(stream, name, ','))
      {
        for (uint8_t t = 0; t < uint8_t(Tag::Count); ++t)
        {
          if (name == tagName(Tag(t)))
          {
            settings.brief.tags.push_back(Tag(t));
          }
        }
      }
    }
    else
    {
      std::cerr << "unknown option " << option << "\n";
      return 1;
    }
  }
  siteSettings.population = {settings.brief.seed, population, population};
  std::filesystem::create_directories(output);
  std::string error;
  GeneratedWorld world;
  if (!generateWorldDirected(settings, world, {}, &error))
  {
    std::cerr << "world generation failed: " << error << "\n";
    return 1;
  }
  WorldSettlementSite site;
  bool selected = selectWorldSettlementSite(world, siteSettings, site, &error);
  std::cout << "site " << (selected ? "selected" : "rejected") << " " << error
            << (selected ? std::string(" class=") + settlementClassName(site.settlementClass) + " plaza=" + std::to_string(site.plazaCell) + " temple=" + std::to_string(site.templeCell) : "") << "\n";
  KingdomPlan kingdom;
  std::string kingdomError;
  bool kingdomOK = selectKingdomSites(world, siteSettings, {}, kingdom, &kingdomError);
  std::cout << "kingdom " << (kingdomOK ? "planned" : "rejected: " + kingdomError) << " settlements=" << kingdom.settlements.size() << " links=" << kingdom.links.size() << "\n";
  // World roads: the Parish-Muller growth over the whole map, seeded by the kingdom links.
  generator::RoadNetwork worldRoads;
  if (kingdomOK)
  {
    generator::Ledger roadLedger;
    std::string roadError;
    if (generator::WorldRoads::grow(world, kingdom, {}, worldRoads, roadLedger, &roadError))
    {
      for (const auto &entry : roadLedger.entries())
      {
        std::cout << "  " << entry.message << "\n";
      }
      std::cout << "world roads segments=" << worldRoads.segments.size() << " modules=" << worldRoads.modules << " pruned=" << worldRoads.pruned << " rotated=" << worldRoads.rotated << " cut=" << worldRoads.cut
                << " snapped=" << worldRoads.snappedCrossing + worldRoads.snappedRoad << " illegal=" << worldRoads.failedIllegal << "\n";
    }
    else
    {
      std::cout << "world roads rejected: " << roadError << "\n";
    }
  }
  for (auto &r : kingdom.rejections)
  {
    std::cout << "  kingdom: " << r << "\n";
  }
  json report;
  report["world"] = {
    {"seed", settings.brief.seed},
    {"extentMetres", settings.brief.extentMetres},
    {"cellSpacingMetres", settings.cellSpacingMetres},
    {"reliefMetres", settings.brief.reliefMetres},
    {"leafCells", world.leafCells},
    {"landCells", world.landCells}};
  report["siteSelected"] = selected;
  report["kingdom"] = json::array();
  for (auto &settled : kingdom.settlements)
  {
    report["kingdom"].push_back(
        {{"id", settled.id},
         {"cell", settled.cell},
         {"role", settlementClassName(settled.role)},
         {"centre", poly({settled.centre})},
         {"population", settled.population},
         {"parentTown", settled.parentTown},
         {"region", settled.regionName}});
  }
  report["kingdomLinks"] = json::array();
  for (auto &link : kingdom.links)
  {
    report["kingdomLinks"].push_back({{"from", link.from}, {"to", link.to}, {"trunk", link.trunk}});
  }
  report["kingdomRejections"] = kingdom.rejections;
  report["siteError"] = error;
  const double extent = settings.brief.extentMetres;

  // Complete world: compose every kingdom settlement (up to --kingdom N) and
  // keep their street and house footprints in world coordinates.
  struct WorldSettlementSketch
  {
    KingdomSettlement settlement;
    std::vector<Poly> houses, districts, plaza;
    std::vector<std::vector<Position>> streets, walls;
    std::vector<Poly> fields;
    bool composed = false;
    std::string note;
  };

  std::vector<WorldSettlementSketch> sketches;
  json worldJson = json::array();
  for (size_t k = 0; kingdomOK && k < kingdom.settlements.size() && k < kingdomCount; ++k)
  {
    const auto &settled = kingdom.settlements[k];
    WorldSettlementSketch sketch;
    sketch.settlement = settled;
    WorldSiteSelectionSettings each = siteSettings;
    each.preferredCell = settled.cell;
    each.population = {settings.brief.seed + settled.id, settled.population, settled.population};
    each.producer = settled.id;
    WorldSettlementSite one;
    std::string oneError;
    if (!selectWorldSettlementSite(world, each, one, &oneError))
    {
      sketch.note = "site: " + oneError;
      sketches.push_back(sketch);
      std::cout << "world " << settlementClassName(settled.role) << " " << settled.id << " rejected: " << oneError << "\n";
      continue;
    }
    CityTerritory oneTerritory;
    if (!generateCityTerritory(one.brief.input, one.brief.suitability, one.brief.settings, one.brief.terrain, oneTerritory, &oneError))
    {
      sketch.note = "territory: " + oneError;
      sketches.push_back(sketch);
      std::cout << "world " << settlementClassName(settled.role) << " " << settled.id << " territory rejected: " << oneError << "\n";
      continue;
    }
    SettlementFabric oneFabric;
    SettlementFabricSettings oneSettings;
    if (settled.alongRoad)
    {
      oneSettings.villageForm = SettlementFabricSettings::VillageForm::Linear;
    }
    bool oneOK = composeSettlementFabric(one, oneTerritory, oneSettings, oneFabric, &oneError);
    auto toWorld = [&](Position p) {
      return Position{p.x + one.origin.x, 0, p.z + one.origin.z};
    };
    for (size_t i = 0; i < oneTerritory.fields.size(); ++i)
    {
      for (const auto &f : oneTerritory.fields[i].fields)
      {
        Poly w;
        for (auto v : f.boundary)
        {
          w.push_back(toWorld(v));
        }
        sketch.fields.push_back(w);
      }
    }
    if (oneOK && oneFabric.layoutAccepted)
    {
      sketch.composed = true;
      for (const auto &o : oneFabric.layout.objects)
      {
        if (o.kind == CityObjectKind::House || o.kind == CityObjectKind::Workshop)
        {
          for (const auto &part : o.parts)
          {
            Poly w;
            for (auto v : part.footprint)
            {
              w.push_back(toWorld(v));
            }
            sketch.houses.push_back(w);
          }
        }
      }
      for (const auto &st : oneFabric.layout.streets)
      {
        std::vector<Position> w;
        for (auto v : st.path)
        {
          w.push_back(toWorld(v));
        }
        sketch.streets.push_back(w);
      }
      for (const auto &wall : oneFabric.layout.walls)
      {
        sketch.walls.push_back({toWorld(wall.a), toWorld(wall.b)});
      }
      for (const auto &d : oneFabric.layout.districts)
      {
        for (const auto &piece : d.pieces)
        {
          Poly w;
          for (auto v : piece)
          {
            w.push_back(toWorld(v));
          }
          (d.use == DistrictUse::Civic ? sketch.plaza : sketch.districts).push_back(w);
        }
      }
    }
    else
    {
      sketch.note = "fabric: " + (oneOK ? std::string("layout rejected") : oneError);
      for (const auto &reason : oneFabric.rejections)
      {
        if (reason.find("layout") != std::string::npos || reason.find("terrace") != std::string::npos || reason.find("gate") != std::string::npos)
        {
          sketch.note += "; " + reason;
        }
      }
    }
    std::cout << "world " << settlementClassName(settled.role) << " " << settled.id << " " << settled.regionName << ": "
              << (sketch.composed ? "streets " + std::to_string(sketch.streets.size()) + " houses " + std::to_string(sketch.houses.size()) : sketch.note) << " fields " << sketch.fields.size() << "\n";
    worldJson.push_back(
        {{"id", settled.id},
         {"role", settlementClassName(settled.role)},
         {"composed", sketch.composed},
         {"streets", sketch.streets.size()},
         {"houses", sketch.houses.size()},
         {"fields", sketch.fields.size()},
         {"note", sketch.note}});
    sketches.push_back(std::move(sketch));
  }
  report["worldSettlements"] = worldJson;
  // Overview: every leaf by biome, rivers, all candidates, the chosen domain.
  {
    const double size = 1400;
    Svg svg(size, size, size / extent);
    for (const auto &n : world.graph.nodes())
    {
      if (n.refined())
      {
        continue;
      }
      Poly p;
      for (auto q : n.area)
      {
        p.push_back({q.x, 0, q.y});
      }
      svg.polygon(p, biomeColour(biomeOf(n)), "#ffffff", .3);
    }
    for (const auto &s : world.rivers.segments)
    {
      svg.polyline({{s.from.x, 0, s.from.y}, {s.to.x, 0, s.to.y}}, "#3d7fd0", .6 + std::sqrt(std::max(0., s.dischargeCumecs)) * .15);
    }
    for (const auto &c : site.candidates)
    {
      svg.circle(c.centre, c.accepted ? 3.5 : 2.5, c.accepted ? "#2ecc40" : "#e0342a", "#222", .4, .85);
    }
    for (const auto &sketch : sketches)
    {
      for (const auto &f : sketch.fields)
      {
        svg.polygon(f, "#d9b85c", "#7a5f10", .3, .9);
      }
      for (const auto &d : sketch.districts)
      {
        svg.polygon(d, "#e0b090", "none", 0, .6);
      }
      for (const auto &p : sketch.plaza)
      {
        svg.polygon(p, "#f0e0a0", "none", 0, .9);
      }
      for (const auto &st : sketch.streets)
      {
        svg.polyline(st, "#f4ecd8", .6);
      }
      for (const auto &h : sketch.houses)
      {
        svg.polygon(h, "#6a3a1a", "none", 0, 1);
      }
      for (const auto &w : sketch.walls)
      {
        svg.polyline(w, "#222", 1.2);
      }
    }
    for (const auto &link : kingdom.links)
    {
      svg.polyline({link.a, link.b}, "#b09a7a", .8, "stroke-dasharray=\"5 4\"");
    }
    for (const auto &sg : worldRoads.segments)
    {
      svg.polyline({sg.a, sg.b}, sg.highway ? "#5a3a1a" : "#8a6a3a", sg.highway ? 2.4 : 1.3);
    }
    for (const auto &settled : kingdom.settlements)
    {
      double radius = settled.role == SettlementClass::Capital ? 11 : settled.role == SettlementClass::Town ? 8 : settled.role == SettlementClass::Village ? 5.5 : 4;
      std::string fill = settled.role == SettlementClass::Capital ? "#ffd400" : settled.role == SettlementClass::Town ? "#ff8c00" : settled.role == SettlementClass::Village ? "#f5f5f5" : "#c8c8c8";
      svg.circle(settled.centre, radius, fill, "#000", 1.4);
      if (settled.role == SettlementClass::Capital || settled.role == SettlementClass::Town)
      {
        svg.text({settled.centre.x, 0, settled.centre.z + 180}, std::string(settlementClassName(settled.role)) + " " + settled.regionName, 14, "#000");
      }
    }
    if (selected)
    {
      svg.polygon(
          {site.origin, {site.origin.x + site.bounds.maxX, 0, site.origin.z}, {site.origin.x + site.bounds.maxX, 0, site.origin.z + site.bounds.maxZ}, {site.origin.x, 0, site.origin.z + site.bounds.maxZ}},
          "none",
          "#ff9900",
          2.5);
      svg.circle(site.centre, 8, "#ffd400", "#000", 1.5);
      svg.text({site.centre.x, 0, site.centre.z + 220}, site.regionName, 16, "#000");
    }
    svg.text({150, 0, 150}, "seed " + std::to_string(settings.brief.seed) + "  " + std::to_string(int(extent / 1000)) + " km  candidates " + std::to_string(site.candidates.size()), 15, "#000", "start");
    std::ofstream(output / "overview.svg") << svg.finish();
  }
  if (!selected)
  {
    std::ofstream(output / "settlement.json") << report.dump(2);
    return 2;
  }
  CityTerritory territory;
  bool composed = generateCityTerritory(site.brief.input, site.brief.suitability, site.brief.settings, site.brief.terrain, territory, &error);
  std::cout << "territory " << (composed ? "composed" : "rejected") << " " << error << "\n";
  report["territoryComposed"] = composed;
  report["territoryError"] = error;
  // Fabric and runtime through the class-based generator chain (per-tile jobs).
  SettlementFabric fabric;
  RuntimeBuildResult built;
  bool fabricOK = false, runtimeOK = false;
  std::string fabricError, runtimeError;
  generator::RuntimeJobResults jobs;
  auto ledger = std::make_shared<generator::Ledger>();
  if (composed)
  {
    SettlementFabricSettings fabricSettings;
    fabricSettings.variantSeed = variant;
    fabricSettings.recipeOverride = recipe;
    fabricSettings.patternOverride = pattern;
    fabricSettings.lotsOnly = lotsOnly;
    fabricSettings.roadSettlement = roads;
    SettlementRuntimeSettings runtimeSettings;
    runtimeSettings.domain = SettlementRuntimeDomain::Full;
    auto worldShared = std::make_shared<const GeneratedWorld>(world);
    WorldSiteGenerationBrief brief{worldShared, siteSettings};
    GenerationSnapshot chain;
    std::string chainError;
    bool ran = runGenerators(
        generator::settlementGeneratorTasks(1, ledger),
        {{{"world.site-brief", 1}, {brief, 1}}, {{"settlement.fabric-settings", 1}, {fabricSettings, 1}}, {{"settlement.runtime-settings", 1}, {runtimeSettings, 1}}},
        settings.brief.seed,
        chain,
        {},
        &chainError);
    std::cout << "generator chain " << (ran ? "ran" : "failed: " + chainError) << " ledger rejected=" << ledger->count(generator::LedgerKind::Rejected) << " omitted=" << ledger->count(generator::LedgerKind::Omitted)
              << " capacity=" << ledger->count(generator::LedgerKind::Capacity) << " repaired=" << ledger->count(generator::LedgerKind::Repaired) << "\n";
    if (chain.count({"settlement.fabric", 1}))
    {
      fabric = std::any_cast<const SettlementFabric &>(chain.at({"settlement.fabric", 1}).value);
      fabricOK = true;
      std::cout << "fabric terrace=" << fabric.terraceAccepted << " layout=" << fabric.layoutAccepted << " streets=" << fabric.layout.streets.size() << " objects=" << fabric.layout.objects.size()
                << " housed=" << fabric.population.housed << " unhoused=" << fabric.population.unhoused << " buildings=" << fabric.population.buildings.size() << " compounds=" << fabric.compounds.size() << " districts=";
      for (const auto &d : fabric.layout.districts)
      {
        std::cout << districtUseName(d.use) << "(" << int(d.area) << ") ";
      }
      std::cout << "\n";
      for (auto &r : fabric.rejections)
      {
        std::cout << "  fabric: " << r << "\n";
      }
    }
    else
    {
      fabricError = chainError;
    }
    if (chain.count({"runtime.results", 1}))
    {
      jobs = std::any_cast<const generator::RuntimeJobResults &>(chain.at({"runtime.results", 1}).value);
      size_t faces = 0, triangles = 0, structures = 0;
      for (auto &job : jobs.jobs)
      {
        if (!job.built)
        {
          continue;
        }
        faces += job.result.assets.levels[0].world.map.faces.size();
        triangles += job.result.assets.levels[0].world.mesh.triangles.size();
        structures += job.result.assets.structures.triangles.size();
      }
      runtimeOK = jobs.built > 0;
      std::cout << "runtime jobs=" << jobs.jobs.size() << " built=" << jobs.built << " faces=" << faces << " triangles=" << triangles << " structures=" << structures << "\n";
      for (auto &job : jobs.jobs)
      {
        if (!job.built)
        {
          std::cout << "  job " << job.name << ": " << job.error << "\n";
        }
      }
    }
    else
    {
      runtimeError = chainError;
    }
  }
  report["fabricComposed"] = fabricOK;
  report["fabricError"] = fabricError;
  report["runtimeBuilt"] = runtimeOK;
  report["runtimeError"] = runtimeError;
  const auto &w = site.brief.suitability;
  const double h = w.settings().spacingMetres;
  // Domain.
  {
    const double width = 1500, scale = width / site.bounds.maxX, height = site.bounds.maxZ * scale;
    Svg svg(width, height, scale);
    svg.rect(0, 0, site.bounds.maxX, site.bounds.maxZ, "#f4f1e8");
    std::vector<double> heights(w.cells().size());
    for (size_t i = 0; i < w.cells().size(); ++i)
    {
      const auto &c = w.cells()[i];
      heights[i] = c.center.y;
      std::string fill;
      if (c.reasons & Wet)
      {
        fill = "#7fb2e5";
      }
      else if (c.reasons & (Steep | Cliff | Brink))
      {
        fill = "#c49a6c";
      }
      else if (c.reasons & Rough)
      {
        fill = "#b8a48a";
      }
      else if (c.reasons & PoorSoil)
      {
        fill = "#e6dca0";
      }
      else if (c.reasons & Saturated)
      {
        fill = "#9fc7b8";
      }
      else if (c.reasons & Flood)
      {
        fill = "#a9c4e8";
      }
      else if (c.reasons & Inaccessible)
      {
        fill = "#d5d5d5";
      }
      else
      {
        continue;
      }
      svg.rect(c.center.x - h * .5, c.center.z - h * .5, h, h, fill, .8);
    }
    double lo = 1e300, hi = -1e300;
    for (auto v : heights)
    {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    double interval = std::max(1., std::round((hi - lo) / 12));
    for (double level = std::ceil(lo / interval) * interval; level <= hi; level += interval)
    {
      for (const auto &line : contourLines(heights, w.columns(), w.rows(), 0, 0, h, level))
      {
        svg.polyline(line, "#6b5a3a", std::fmod(level, interval * 5) == 0 ? 1 : .5, "stroke-opacity=\"0.6\"");
      }
    }
    for (const auto &r : site.regions)
    {
      std::string fill = r.cover == RuralLandCover::Forest ? "#4f7a3a" : r.cover == RuralLandCover::Rock ? "#8a8a8a" : r.cover == RuralLandCover::Water ? "#5f9ad6" : r.reserved ? "#e3d9c0" : "none";
      svg.polygon(r.boundary, fill, "#8c7a4a", .8, .5);
    }
    for (const auto &p : territory.rural.patches)
    {
      svg.polygon(p.boundary, "#e9d58a", "#9a7d1c", 1.2, .55);
    }
    for (const auto &f : territory.fields)
    {
      for (const auto &field : f.fields)
      {
        svg.polygon(field.boundary, "#d9b85c", "#7a5f10", .6, .85);
      }
    }
    for (const auto &c : territory.surfaces.contours)
    {
      if (c.role == SurfaceRole::Road)
      {
        svg.polygon(c.loop, "#b28b5a", "#6a4f2a", .3, .95);
      }
    }
    for (const auto &p : territory.rural.patches)
    {
      svg.polyline(p.access, "#7a5f10", 1.2, "stroke-dasharray=\"4 3\"");
    }
    for (const auto &hold : territory.holdings)
    {
      svg.polygon(hold.yard, "#a0703c", "#5a3a1a", .8, .9);
      for (const auto &b : hold.buildings)
      {
        svg.polygon(b.footprint, "#4a2e12", "#000", .4);
      }
    }
    for (const auto &cell : site.brief.input.cells)
    {
      svg.polygon(cell.boundary, cell.buildable ? "none" : "#c0c0c0", "#444", .7, .5);
    }
    for (const auto &d : territory.regions.districts)
    {
      for (const auto &piece : d.pieces)
      {
        svg.polygon(piece, useColour(d.use), "#222", 1, .75);
      }
      svg.polyline(d.access, "#111", 1.5, "stroke-dasharray=\"6 4\"");
      if (!d.pieces.empty())
      {
        svg.text(centroid(d.pieces.front()), districtUseName(d.use), 11, "#000");
      }
    }
    if (fabricOK)
    {
      svg.polygon(fabric.polygon.outer, "#efe6c8", "#5a4a2a", 1.5, .9);
      for (const auto &c : fabric.layout.contours)
      {
        if (c.material == 1)
        {
          svg.polygon(c.loop, "#c9b28a", "none", 0, 1);
        }
      }
      for (const auto &o : fabric.layout.objects)
      {
        if (o.kind == CityObjectKind::House || o.kind == CityObjectKind::Workshop)
        {
          for (const auto &part : o.parts)
          {
            svg.polygon(part.footprint, "#8a5a3a", "#3a2a1a", .4, 1);
          }
        }
      }
      for (const auto &compound : fabric.compounds)
      {
        for (const auto &wall : compound.walls)
        {
          svg.polyline({wall.a, wall.b}, wall.gate ? "#f0d080" : "#202020", wall.thickness * 1.5 + .5);
        }
        for (const auto &tower : compound.towers)
        {
          svg.polygon(tower.footprint, "#404040", "#000", .4, 1);
        }
        for (const auto &wing : compound.wings)
        {
          svg.polygon(wing.brief.shell, wing.planned ? "#7a3a2a" : "#d04040", "#2a1a0a", .4, 1);
        }
      }
      for (const auto &a : fabric.arteries)
      {
        svg.polyline({a.a, a.b}, "#3a2a1a", 2.2);
      }
      for (const auto &approach : fabric.approaches)
      {
        svg.polyline(approach, "#6a4f2a", 2.6, "stroke-dasharray=\"8 5\"");
      }
      for (const auto &g : fabric.gates)
      {
        svg.circle(g.position, 5, "#ffffff", "#000", 1.5);
      }
    }
    for (const auto &job : jobs.jobs)
    {
      svg.polygon({{job.minX, 0, job.minZ}, {job.maxX, 0, job.minZ}, {job.maxX, 0, job.maxZ}, {job.minX, 0, job.maxZ}}, "none", job.built ? "#2a6f2a" : "#c0392b", 1.2, 1);
      svg.text({job.minX + 12, 0, job.maxZ - 22}, job.built ? "faces " + std::to_string(job.result.assets.levels[0].world.map.faces.size()) : "capacity", 12, job.built ? "#2a6f2a" : "#c0392b", "start");
    }
    for (const auto &g : site.brief.input.gateCells)
    {
      for (const auto &cell : site.brief.input.cells)
      {
        if (cell.id == g)
        {
          svg.circle(centroid(cell.boundary), 4, "#ffffff", "#444", 1);
        }
      }
    }
    for (const auto &src : site.brief.input.ruralSources)
    {
      svg.circle(src.exit, 6, "#ffcc00", "#000", 1.5);
    }
    svg.circle(site.localCentre, 8, "#e0342a", "#000", 1.5);
    svg.text(
        {20, 0, site.bounds.maxZ - 30},
        "site " + std::to_string(site.cell) + " (" + site.regionName + ")  contour " + std::to_string(int(interval)) + " m  population " + std::to_string(site.demand.population),
        16,
        "#000",
        "start");
    std::ofstream(output / "domain.svg") << svg.finish();
  }
  // JSON ledger.
  report["site"] = {
    {"id", site.id},
    {"cell", site.cell},
    {"region", site.regionName},
    {"centre", poly({site.centre})},
    {"origin", poly({site.origin})},
    {"size", {site.bounds.maxX, site.bounds.maxZ}},
    {"attempts", site.attempts},
    {"population", site.demand.population},
    {"households", site.households.size()},
    {"planningArea", site.planningArea},
    {"hinterlandArea", site.hinterlandArea},
    {"openArea", site.openArea}};
  for (const auto &c : site.candidates)
  {
    report["candidates"].push_back(
        {{"cell", c.cell},
         {"centre", poly({c.centre})},
         {"score", c.score},
         {"gradient", c.gradient},
         {"hinterlandGradient", c.hinterlandGradient},
         {"waterDistance", c.waterDistance},
         {"fertility", c.fertility},
         {"biome", biomeName(Biome(c.biome))},
         {"accepted", c.accepted},
         {"rejection", c.rejection}});
  }
  report["omissions"] = site.rejections;
  for (const auto &cell : site.brief.input.cells)
  {
    report["planningCells"].push_back(
        {{"id", cell.id},
         {"boundary", poly(cell.boundary)},
         {"slope", cell.slope},
         {"floodRisk", cell.floodRisk},
         {"fertility", cell.fertility},
         {"defense", cell.defense},
         {"buildable", cell.buildable},
         {"allocatable", cell.allocatable},
         {"navigableShore", cell.navigableShore}});
  }
  for (const auto &r : site.regions)
  {
    report["regions"].push_back({{"id", r.id}, {"boundary", poly(r.boundary)}, {"cover", int(r.cover)}});
  }
  for (const auto &src : site.brief.input.ruralSources)
  {
    report["ruralSources"].push_back({{"id", src.id}, {"exit", poly({src.exit})}, {"areaBudget", src.areaBudget}});
  }
  report["gateCells"] = site.brief.input.gateCells;
  for (const auto &d : site.brief.input.districts)
  {
    report["districtRequests"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"minimumArea", d.minimumArea}, {"targetArea", d.targetArea}});
  }
  report["suitability"] = {{"columns", w.columns()}, {"rows", w.rows()}, {"spacing", h}, {"fingerprint", w.fingerprint()}};
  {
    std::string reasons;
    for (const auto &c : w.cells())
    {
      reasons += c.reasons ? '1' : '0';
    }
    report["suitability"]["eligible"] = reasons;
  }
  if (composed)
  {
    report["territory"] = {
      {"urbanArea", territory.regions.assignedArea},
      {"cropArea", territory.cropArea},
      {"farmyardArea", territory.farmyardArea},
      {"patches", territory.rural.patches.size()},
      {"holdings", territory.holdings.size()},
      {"contours", territory.surfaces.contours.size()},
      {"eligibleArea", territory.rural.eligibleArea},
      {"patchArea", territory.rural.patchArea},
      {"unallocatedArea", territory.rural.unallocatedArea}};
    for (const auto &d : territory.regions.districts)
    {
      json pieces = json::array();
      for (auto q : d.pieces)
      {
        pieces.push_back(poly(q));
      }
      report["districts"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"cells", d.cells}, {"pieces", pieces}, {"access", poly(d.access)}, {"area", d.area}, {"shortfall", d.unmetTargetArea}});
    }
    for (const auto &o : territory.regions.omissions)
    {
      report["districtOmissions"].push_back({{"request", o.request}, {"reason", o.reason}});
    }
    for (size_t i = 0; i < territory.rural.patches.size(); ++i)
    {
      const auto &p = territory.rural.patches[i];
      json fields = json::array();
      for (const auto &f : territory.fields[i].fields)
      {
        fields.push_back({{"boundary", poly(f.boundary)}, {"area", f.area}});
      }
      report["patches"].push_back(
          {{"id", p.id},
           {"source", p.source},
           {"originalRegion", p.originalRegion},
           {"boundary", poly(p.boundary)},
           {"access", poly(p.access)},
           {"area", p.area},
           {"cropArea", territory.fields[i].cropArea},
           {"fields", fields}});
    }
    for (const auto &hold : territory.holdings)
    {
      json buildings = json::array();
      for (const auto &b : hold.buildings)
      {
        buildings.push_back({{"id", b.id}, {"use", int(b.use)}, {"footprint", poly(b.footprint)}, {"height", b.height}});
      }
      report["holdings"].push_back({{"patch", hold.patch}, {"yard", poly(hold.yard)}, {"buildings", buildings}});
    }
    report["ruralRejections"] = territory.rural.rejections;
    report["budgets"] = json::array();
    for (const auto &b : territory.rural.budgets)
    {
      report["budgets"].push_back({{"source", b.source}, {"requested", b.requested}, {"allocated", b.allocated}, {"shortfall", b.shortfall}});
    }
  }
  std::ofstream(output / "settlement.json") << report.dump(2);
  // Existing Python renderer schemas: city-territory.json (render_city_territory.territory)
  // and village-layout.json (render_city_layout_comparison.panel).
  if (composed)
  {
    json t;
    t["revision"] = territory.revision;
    t["cropArea"] = territory.cropArea;
    t["farmyardArea"] = territory.farmyardArea;
    t["rejections"] = territory.rural.rejections;
    t["bounds"] = {0, 0, site.bounds.maxX, site.bounds.maxZ};
    t["regions"] = json::array();
    for (const auto &d : territory.regions.districts)
    {
      json pieces = json::array();
      for (auto q : d.pieces)
      {
        pieces.push_back(poly(q));
      }
      t["regions"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"pieces", pieces}, {"access", poly(d.access)}, {"area", d.area}, {"shortfall", d.unmetTargetArea}});
    }
    t["patches"] = json::array();
    for (size_t i = 0; i < territory.rural.patches.size(); ++i)
    {
      auto &r = territory.rural.patches[i];
      json fields = json::array();
      for (auto &f : territory.fields[i].fields)
      {
        fields.push_back({{"boundary", poly(f.boundary)}, {"access", poly(f.access)}, {"area", f.area}});
      }
      t["patches"].push_back(
          {{"id", r.id},
           {"source", r.source},
           {"originalRegion", r.originalRegion},
           {"boundary", poly(r.boundary)},
           {"access", poly(r.access)},
           {"area", r.area},
           {"fields", fields},
           {"cropArea", territory.fields[i].cropArea}});
    }
    t["holdings"] = json::array();
    for (auto &hold : territory.holdings)
    {
      json buildings = json::array();
      for (auto &b : hold.buildings)
      {
        buildings.push_back({{"id", b.id}, {"use", int(b.use)}, {"footprint", poly(b.footprint)}, {"entrance", poly({b.entrance})}, {"height", b.height}});
      }
      t["holdings"].push_back({{"patch", hold.patch}, {"source", hold.source}, {"yard", poly(hold.yard)}, {"access", poly(hold.access)}, {"buildings", buildings}});
    }
    t["contours"] = json::array();
    for (auto c : territory.surfaces.contours)
    {
      t["contours"].push_back({{"id", c.id}, {"material", c.material}, {"role", int(c.role)}, {"loop", poly(c.loop)}});
    }
    t["originalRegions"] = json::array();
    for (auto &r : site.regions)
    {
      t["originalRegions"].push_back({{"id", r.id}, {"boundary", poly(r.boundary)}, {"cover", int(r.cover)}, {"reserved", r.reserved}});
    }
    t["occupied"] = json::array();
    t["suitabilitySource"] = "actual generated world: map surface elevation, explicit ocean/lake/river water, climate moisture, biome fertility class";
    auto encodeLayout = [&](const CityLayout &c) {
      json j;
      j["revision"] = c.revision;
      j["envelope"] = poly(c.envelope.outer);
      j["acceptedEnvelope"] = poly(c.envelope.outer);
      j["holes"] = json::array();
      for (auto &hole : c.envelope.holes)
      {
        j["holes"].push_back(poly(hole));
      }
      j["suburbs"] = json::array();
      for (auto &p : c.suburbs)
      {
        j["suburbs"].push_back(poly(p));
      }
      j["reservations"] = json::array();
      for (auto &p : c.reservations)
      {
        j["reservations"].push_back(poly(p));
      }
      j["towers"] = json::array();
      for (auto &tw : c.towers)
      {
        j["towers"].push_back({{"id", tw.id}, {"footprint", poly(tw.footprint)}, {"height", tw.height}});
      }
      j["districts"] = json::array();
      for (auto &d : c.districts)
      {
        json pieces = json::array();
        for (auto &p : d.pieces)
        {
          pieces.push_back(poly(p));
        }
        j["districts"].push_back({{"id", d.id}, {"use", int(d.use)}, {"name", districtUseName(d.use)}, {"pieces", pieces}, {"area", d.area}, {"access", poly(d.access)}});
      }
      j["wards"] = json::array();
      for (auto &ward : c.wards)
      {
        json pieces = json::array();
        for (auto &p : ward.pieces)
        {
          pieces.push_back(poly(p));
        }
        j["wards"].push_back({{"id", ward.id}, {"pieces", pieces}});
      }
      j["streets"] = json::array();
      for (auto &st : c.streets)
      {
        j["streets"].push_back(
            {{"id", st.id}, {"a", {st.a.x, st.a.y, st.a.z}}, {"b", {st.b.x, st.b.y, st.b.z}}, {"width", st.halfWidth * 2}, {"arterial", st.arterial}, {"hierarchy", st.hierarchy}, {"path", poly(st.path)}});
      }
      j["objects"] = json::array();
      for (auto &o : c.objects)
      {
        json parts = json::array();
        for (auto &part : o.parts)
        {
          parts.push_back({{"footprint", poly(part.footprint)}, {"height", part.height}});
        }
        j["objects"].push_back(
            {{"id", o.id},
             {"ward", o.ward},
             {"block", o.block},
             {"kind", int(o.kind)},
             {"footprint", poly(o.footprint)},
             {"clearance", poly(o.clearance)},
             {"entrance", {o.entrance.x, o.entrance.y, o.entrance.z}},
             {"access", {o.access.x, o.access.y, o.access.z}},
             {"foundation", o.foundation},
             {"height", o.height},
             {"buildingType", int(o.buildingType)},
             {"parts", parts},
             {"courtyard", poly(o.courtyard)},
             {"compactFallback", o.compactFallback}});
      }
      j["blocks"] = json::array();
      for (auto &b : c.blocks)
      {
        json courts = json::array();
        for (auto &p : b.courtyards)
        {
          courts.push_back(poly(p.outer));
        }
        json lots = json::array(), openLots = json::array();
        for (auto &p : b.lots)
        {
          lots.push_back(poly(p));
        }
        for (auto &p : b.openLots)
        {
          openLots.push_back(poly(p));
        }
        j["blocks"].push_back({{"id", b.id}, {"courtyards", courts}, {"use", int(b.use)}, {"district", b.district}, {"rejection", b.rejection}, {"outer", poly(b.boundary.outer)}, {"lots", lots}, {"openLots", openLots}});
      }
      j["walls"] = json::array();
      for (auto &wall : c.walls)
      {
        j["walls"].push_back({poly({wall.a, wall.b}), wall.halfWidth});
      }
      j["contours"] = json::array();
      for (auto &c2 : c.contours)
      {
        j["contours"].push_back({{"id", c2.id}, {"material", c2.material}, {"role", int(c2.role)}, {"loop", poly(c2.loop)}});
      }
      j["subdivisionCells"] = c.subdivisionCells;
      j["rejected"] = c.rejectedAreas;
      return j;
    };
    if (fabricOK && fabric.layoutAccepted)
    {
      auto layout = encodeLayout(fabric.layout);
      t["city"] = layout;
      layout["terraceLevel"] = fabric.terraceLevel;
      layout["terraceStep"] = fabric.terraceStep;
      layout["housed"] = fabric.population.housed;
      layout["unhoused"] = fabric.population.unhoused;
      layout["buildings"] = fabric.population.buildings.size();
      layout["rejections"] = fabric.rejections;
      std::ofstream(output / "village-layout.json") << layout.dump(2);
      // Fitted interiors in the v9 director schema, for render_settlement_director.floor_panel.
      size_t exported = 0;
      std::vector<const SettlementBuilding *> ordered;
      for (const auto &building : fabric.population.buildings)
      {
        ordered.push_back(&building);
      }
      std::stable_sort(ordered.begin(), ordered.end(), [](const SettlementBuilding *a, const SettlementBuilding *b) {
        if (a->interior.floors.size() != b->interior.floors.size())
        {
          return a->interior.floors.size() > b->interior.floors.size();
        }
        if (a->programme != b->programme)
        {
          return a->programme < b->programme;
        }
        return a->interior.rooms.size() > b->interior.rooms.size();
      });
      std::map<std::string, int> perProgramme;
      for (const auto *buildingPointer : ordered)
      {
        const auto &building = *buildingPointer;
        const auto &d = building.interior;
        if (d.rooms.empty() || exported >= 8 || perProgramme[building.programme]++ >= 3)
        {
          continue;
        }
        json j = {{"id", d.id}, {"use", int(d.use)}, {"residents", d.residentCapacity}, {"workers", d.workerCapacity}, {"programme", building.programme}, {"fallback", d.fallback}, {"note", d.note}};
        j["rooms"] = json::array();
        j["walls"] = json::array();
        j["doors"] = json::array();
        j["floors"] = json::array();
        j["stairs"] = json::array();
        for (auto &r : d.rooms)
        {
          j["rooms"].push_back({{"id", r.id}, {"floor", r.floor}, {"use", int(r.use)}, {"boundary", poly(r.boundary)}, {"area", r.usableArea}});
        }
        for (auto &wl : d.walls)
        {
          j["walls"].push_back({{"id", wl.id}, {"floor", wl.floor}, {"path", poly({wl.a, wl.b})}, {"thickness", wl.thickness}, {"exterior", wl.exterior}});
        }
        for (auto &dr : d.doors)
        {
          j["doors"].push_back(
              {{"wall", dr.wall}, {"room", dr.room}, {"floor", dr.floor}, {"center", poly({dr.center})}, {"width", dr.width}, {"private", dr.privateAccess}, {"exterior", dr.exterior}, {"shopOpening", dr.shopOpening}});
        }
        for (auto &f : d.floors)
        {
          json c = json::array(), sl = json::array();
          for (auto &q : f.circulation)
          {
            c.push_back(poly(q));
          }
          for (auto &q : f.slabs)
          {
            sl.push_back(poly(q));
          }
          j["floors"].push_back(
              {{"elevation", f.elevation},
               {"height", f.height},
               {"gross", f.grossArea},
               {"roomArea", f.roomArea},
               {"circulationArea", f.circulationArea},
               {"wallArea", f.wallArea},
               {"coreArea", f.coreArea},
               {"circulation", c},
               {"slabs", sl}});
        }
        for (auto &st : d.stairs)
        {
          j["stairs"].push_back(
              {{"opening", poly(st.opening)}, {"lower", poly(st.lowerLanding)}, {"upper", poly(st.upperLanding)}, {"risers", st.risers}, {"rise", st.rise}, {"tread", st.tread}, {"private", st.privateAccess}});
        }
        std::ofstream(output / ("dwelling-" + std::to_string(exported++) + ".json")) << j.dump(2);
      }
    }
    std::ofstream(output / "city-territory.json") << t.dump(2);
  }
  // Block grammar (B1-B3) on the first street blocks of a planned layout, or
  // on ward pieces of an organic one; rendered as one SVG sheet.
  if (fabricOK && fabric.layoutAccepted)
  {
    std::vector<std::pair<uint64_t, std::vector<Position>>> candidates;
    for (const auto &blk : fabric.layout.blocks)
    {
      if (blk.boundary.outer.size() >= 3 && blk.courtyards.empty())
      {
        candidates.push_back({blk.id, blk.boundary.outer});
      }
    }
    if (candidates.empty())
    {
      for (const auto &ward : fabric.layout.wards)
      {
        for (const auto &piece : ward.pieces)
        {
          candidates.push_back({ward.id, piece});
        }
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](auto &a, auto &b) {
      return std::abs(signedAreaOf(a.second)) > std::abs(signedAreaOf(b.second));
    });
    generator::Ledger blockLedger;
    json blocksJson = json::array();
    const double sheet = 1400;
    std::vector<generator::BlockPlan> plans;
    for (size_t i = 0; i < candidates.size() && plans.size() < 4; ++i)
    {
      auto outline = candidates[i].second;
      if (outline.size() > 64 || std::abs(signedAreaOf(outline)) < 400)
      {
        continue;
      }
      if (signedAreaOf(outline) < 0)
      {
        std::reverse(outline.begin(), outline.end());
      }
      std::vector<bool> streetSides(outline.size(), false);
      for (size_t k = 0; k < outline.size(); ++k)
      {
        Position mid{(outline[k].x + outline[(k + 1) % outline.size()].x) * .5, 0, (outline[k].z + outline[(k + 1) % outline.size()].z) * .5};
        for (const auto &st : fabric.layout.streets)
        {
          for (size_t j = 1; j < st.path.size(); ++j)
          {
            auto a = st.path[j - 1], b = st.path[j];
            double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
            double t = l2 > 0 ? std::clamp(((mid.x - a.x) * dx + (mid.z - a.z) * dz) / l2, 0., 1.) : 0;
            if (std::hypot(mid.x - a.x - dx * t, mid.z - a.z - dz * t) <= st.halfWidth + 1.5)
            {
              streetSides[k] = true;
            }
          }
        }
      }
      generator::BlockGrammarSettings settings;
      settings.seed = settings.seed + candidates[i].first;
      generator::BlockPlan plan;
      std::string blockError;
      if (!generator::planBlockGrammar(candidates[i].first, outline, streetSides, fabric.terrain, settings, plan, blockLedger, &blockError))
      {
        std::cout << "block " << candidates[i].first << " grammar rejected: " << blockError << "\n";
        continue;
      }
      std::cout << "block " << candidates[i].first << " cells=" << plan.cells.size() << " lots=" << plan.lots << " alleys=" << plan.alleys << " stairs=" << plan.stairs << " courts=" << plan.courts
                << " streets=" << plan.streets << " inaccessible=" << plan.inaccessibleLots << " restarts=" << plan.restarts << " solved=" << plan.solved << "\n";
      json cells = json::array();
      for (const auto &c : plan.cells)
      {
        cells.push_back({{"id", c.id}, {"ring", poly(c.ring)}, {"tile", int(c.tile)}, {"elevation", c.elevation}, {"accessible", c.accessible}});
      }
      blocksJson.push_back(
          {{"block", candidates[i].first}, {"outline", poly(outline)}, {"cells", cells}, {"lots", plan.lots}, {"alleys", plan.alleys}, {"stairs", plan.stairs}, {"courts", plan.courts}, {"solved", plan.solved}});
      plans.push_back(std::move(plan));
    }
    if (!plans.empty())
    {
      double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
      for (auto &plan : plans)
      {
        for (auto &c : plan.cells)
        {
          for (auto v : c.ring)
          {
            x0 = std::min(x0, v.x);
            z0 = std::min(z0, v.z);
            x1 = std::max(x1, v.x);
            z1 = std::max(z1, v.z);
          }
        }
      }
      double scale = sheet / std::max(x1 - x0, z1 - z0);
      Svg svg(sheet, sheet, scale);
      svg.rect(0, 0, sheet / scale, sheet / scale, "#f4f1e8");
      for (auto &plan : plans)
      {
        for (auto &c : plan.cells)
        {
          std::string fill = c.tile == generator::BlockTile::Street  ? "#c9b28a"
                             : c.tile == generator::BlockTile::Alley ? "#e6dcc3"
                             : c.tile == generator::BlockTile::Stair ? "#b07a4a"
                             : c.tile == generator::BlockTile::Lot   ? (c.accessible ? "#8a5a3a" : "#d04040")
                             : c.tile == generator::BlockTile::Court ? "#9fb98a"
                                                                     : "#ffffff";
          Poly local;
          for (auto v : c.ring)
          {
            local.push_back({v.x - x0, 0, v.z - z0});
          }
          svg.polygon(local, fill, "#3a2a1a", .5, 1);
        }
      }
      svg.text({10, 0, (z1 - z0) - 12 / scale}, "block grammar: lots (brown, red = no access), alleys (pale), stairs (dark), courts (green), street cells (tan)", 14, "#000", "start");
      std::ofstream(output / "blocks.svg") << svg.finish();
      std::ofstream(output / "blocks.json") << blocksJson.dump(2);
    }
  }
  // Compound sheet: each castle/manor/church/hall precinct with walls, towers and planned wings.
  if (fabricOK && !fabric.compounds.empty())
  {
    const double panel = 700;
    const size_t cols = std::min<size_t>(3, fabric.compounds.size());
    const size_t rowsN = (fabric.compounds.size() + cols - 1) / cols;
    Svg svg(panel * cols, panel * rowsN, 1);
    svg.rect(0, 0, panel * cols, panel * rowsN, "#f4f1e8");
    for (size_t i = 0; i < fabric.compounds.size(); ++i)
    {
      const auto &c = fabric.compounds[i];
      double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
      for (auto v : c.precinct)
      {
        x0 = std::min(x0, v.x);
        z0 = std::min(z0, v.z);
        x1 = std::max(x1, v.x);
        z1 = std::max(z1, v.z);
      }
      const double scale = (panel - 80) / std::max(x1 - x0, z1 - z0);
      const double ox = (i % cols) * panel + 40, oz = (rowsN - 1 - i / cols) * panel + 40;
      auto map = [&](Position p) {
        return Position{ox + (p.x - x0) * scale, 0, oz + (p.z - z0) * scale};
      };
      auto mapPoly = [&](const Poly &p) {
        Poly out;
        for (auto v : p)
        {
          out.push_back(map(v));
        }
        return out;
      };
      svg.polygon(mapPoly(c.precinct), "#e8e0c8", "#8a7a5a", 1, 1);
      for (const auto &court : c.courts)
      {
        svg.polygon(mapPoly(court), "#d8cfa8", "none", 0, .8);
      }
      for (const auto &wing : c.wings)
      {
        svg.polygon(mapPoly(wing.brief.shell), wing.planned ? "#8a5a3a" : "#d04040", "#2a1a0a", 1, 1);
        for (const auto &room : wing.plan.rooms)
        {
          if (room.floor == 0)
          {
            svg.polygon(mapPoly(room.boundary), "#c9a878", "#5a3a1a", .6, 1);
          }
        }
        auto cshell = centroid(wing.brief.shell);
        svg.text(map(cshell), wing.role, 13, "#fff");
      }
      for (const auto &tower : c.towers)
      {
        svg.polygon(mapPoly(tower.footprint), "#404040", "#000", .6, 1);
      }
      for (const auto &wall : c.walls)
      {
        svg.polyline(mapPoly({wall.a, wall.b}), wall.gate ? "#f0d080" : "#202020", std::max(2., wall.thickness * scale));
      }
      const char *kindName = c.kind == generator::CompoundKind::Castle ? "castle" : c.kind == generator::CompoundKind::Manor ? "manor" : c.kind == generator::CompoundKind::Church ? "church" : "hall";
      svg.text(
          {ox, 0, oz + (z1 - z0) * scale + 24},
          std::string(kindName) + " " + std::to_string(c.id) + ": " + std::to_string(c.plannedWings) + "/" + std::to_string(c.wings.size()) + " wings planned, " + std::to_string(c.towers.size()) + " towers",
          14,
          "#000",
          "start");
    }
    std::ofstream(output / "compounds.svg") << svg.finish();
  }
  // HTML.
  {
    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>World settlement W0 "
            "preview</title><style>body{font-family:Helvetica,Arial,sans-serif;margin:20px;color:#222}table{border-collapse:collapse;margin:8px 0}td,th{border:1px solid #ccc;padding:3px "
            "8px;font-size:13px;text-align:left}img{max-width:100%;border:1px solid #999}.legend span{display:inline-block;width:14px;height:14px;margin-right:4px;vertical-align:middle;border:1px solid "
            "#666}</style></head><body>";
    html << "<h1>Actual-world settlement, W0 input adapter</h1><p>Seed " << settings.brief.seed << ", " << extent / 1000 << " km world, " << settings.cellSpacingMetres << " m leaf cells, relief "
         << settings.brief.reliefMetres << " m. Site cell " << site.cell << " in " << site.regionName << ", population brief " << site.demand.population << ".</p>";
    html << "<h2>World overview</h2><p>Leaf cells by biome, rivers by discharge. Green dots: accepted site candidates; red: rejected with a recorded reason. Orange box: the settlement domain (the urban cell plus its "
            "neighbouring map cells).</p><img src=\"overview.svg\">";
    html << "<h2>Settlement domain (local frame, " << site.bounds.maxX << " x " << site.bounds.maxZ
         << " m)</h2><p class=\"legend\"><span style=\"background:#7fb2e5\"></span>explicit water <span style=\"background:#c49a6c\"></span>steep <span style=\"background:#b8a48a\"></span>rough <span "
            "style=\"background:#e6dca0\"></span>poor soil <span style=\"background:#9fc7b8\"></span>saturated <span style=\"background:#d5d5d5\"></span>unreachable <span style=\"background:#4f7a3a\"></span>forest "
            "region <span style=\"background:#8a8a8a\"></span>rock region <span style=\"background:#e9d58a\"></span>allocated farmland <span style=\"background:#d9b85c\"></span>crop field <span "
            "style=\"background:#b28b5a\"></span>lane <span style=\"background:#a0703c\"></span>farmyard <span style=\"background:#e07b5f\"></span>residential <span style=\"background:#d9a441\"></span>craftsmen <span "
            "style=\"background:#c56f9a\"></span>worker housing <span style=\"background:#8e6bd6\"></span>temple <span style=\"background:#4a8fd6\"></span>civic. White circles: gate cells; yellow: road exits; red: site "
            "centre; dashed black: planning access; dashed brown: farm access; thin brown: elevation contours.</p><img src=\"domain.svg\">";
    html << "<h2>Ledger</h2><table><tr><th>Metric</th><th>Value</th></tr>";
    html << "<tr><td>Candidates evaluated</td><td>" << site.candidates.size() << "</td></tr><tr><td>Planning cells</td><td>" << site.brief.input.cells.size() << "</td></tr><tr><td>Hinterland regions</td><td>"
         << site.regions.size() << "</td></tr><tr><td>Road exits / gates</td><td>" << site.brief.input.ruralSources.size() << " / " << site.brief.input.gateCells.size() << "</td></tr>";
    if (composed)
    {
      html << "<tr><td>Districts allocated</td><td>" << territory.regions.districts.size() << " (" << territory.regions.assignedArea << " m&sup2;)</td></tr><tr><td>Farm patches / fields / crop area</td><td>"
           << territory.rural.patches.size() << " / " <<
          [&] {
            size_t n = 0;
            for (auto &f : territory.fields)
            {
              n += f.fields.size();
            }
            return n;
          }()
           << " / " << territory.cropArea / 1e4 << " ha</td></tr><tr><td>Farmsteads</td><td>" << territory.holdings.size() << "</td></tr><tr><td>Rural rejections</td><td>" << territory.rural.rejections.size()
           << "</td></tr>";
    }
    else
    {
      html << "<tr><td>Territory</td><td>rejected: " << error << "</td></tr>";
    }
    html << "</table><h3>Rejected site candidates (first 12)</h3><table><tr><th>Cell</th><th>Score</th><th>Reason</th></tr>";
    size_t shown = 0;
    for (const auto &c : site.candidates)
    {
      if (!c.accepted && shown++ < 12)
      {
        html << "<tr><td>" << c.cell << "</td><td>" << c.score << "</td><td>" << c.rejection << "</td></tr>";
      }
    }
    html << "</table>";
    if (composed)
    {
      std::map<std::string, int> kinds;
      for (auto r : territory.rural.rejections)
      {
        auto pos = r.find(' ', r.find("region ") + 7);
        kinds[pos == std::string::npos ? r : "original region N " + r.substr(pos + 1)]++;
      }
      html << "<h3>Rural rejections by kind</h3><table><tr><th>Kind</th><th>Count</th></tr>";
      for (auto &[k, v] : kinds)
      {
        html << "<tr><td>" << k << "</td><td>" << v << "</td></tr>";
      }
      html << "</table>";
    }
    html << "</body></html>";
    std::ofstream(output / "index.html") << html.str();
  }
  std::cout << "wrote " << output << "\n";
  report["jobs"] = json::array();
  for (auto &job : jobs.jobs)
  {
    report["jobs"].push_back(
        {{"name", job.name}, {"built", job.built}, {"error", job.error}, {"bounds", {job.minX, job.minZ, job.maxX, job.maxZ}}, {"faces", job.built ? job.result.assets.levels[0].world.map.faces.size() : 0}});
  }
  std::ofstream(output / "settlement.json") << report.dump(2);
  return composed ? 0 : 3;
}
