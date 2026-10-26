// Renders the actual-world settlement integration (W0) as SVG/HTML/JSON:
// the world overview with every evaluated site candidate, and the local
// settlement domain with suitability, regions, districts, farmland, exits
// and gates. Vector output is the review evidence; PNGs are rasterised
// from the SVGs by the caller.
#include "json.hpp"
#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapHierarchy.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/WorldSettlementSiteAdapter.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
using namespace procedural::world;
using namespace procedural::world::rewrite;
using nlohmann::json;
namespace
{
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
      return;
    out << "<polygon points=\"";
    for (auto q : p)
      out << x(q.x) << "," << y(q.z) << " ";
    out << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << strokeWidth << "\" fill-opacity=\"" << opacity << "\"/>\n";
  }
  void polyline(const Poly &p, const std::string &stroke, double strokeWidth, const std::string &extra = "")
  {
    if (p.size() < 2)
      return;
    out << "<polyline points=\"";
    for (auto q : p)
      out << x(q.x) << "," << y(q.z) << " ";
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
    a.push_back({v.x, v.y, v.z});
  return a;
}
// Marching squares over a regular grid of heights (cell centres).
std::vector<Poly> contourLines(const std::vector<double> &h, uint32_t cols, uint32_t rows, double x0, double z0, double spacing, double level)
{
  std::vector<Poly> lines;
  auto at = [&](uint32_t i, uint32_t j)
  {
    return h[j * cols + i];
  };
  auto pos = [&](double i, double j)
  {
    return Position{x0 + (i + .5) * spacing, 0, z0 + (j + .5) * spacing};
  };
  for (uint32_t j = 0; j + 1 < rows; ++j)
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
        lines.push_back({cuts[0], cuts[1]});
      else if (cuts.size() == 4)
      {
        lines.push_back({cuts[0], cuts[1]});
        lines.push_back({cuts[2], cuts[3]});
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
  std::filesystem::path output = "settlement-preview";
  for (int i = 1; i + 1 < argc; i += 2)
  {
    std::string option = argv[i], value = argv[i + 1];
    if (option == "--seed")
      settings.brief.seed = std::stoull(value);
    else if (option == "--extent")
      settings.brief.extentMetres = std::stod(value) * 1000;
    else if (option == "--spacing")
      settings.cellSpacingMetres = std::stod(value) * 1000;
    else if (option == "--relief")
      settings.brief.reliefMetres = std::stod(value);
    else if (option == "--levels")
      settings.hierarchyLevels = uint32_t(std::stoul(value));
    else if (option == "--population")
      population = uint32_t(std::stoul(value));
    else if (option == "--cell")
      siteSettings.preferredCell = uint32_t(std::stoul(value));
    else if (option == "--output")
      output = value;
    else if (option == "--tags")
    {
      settings.brief.tags.clear();
      std::stringstream stream(value);
      std::string name;
      while (std::getline(stream, name, ','))
        for (uint8_t t = 0; t < uint8_t(Tag::Count); ++t)
          if (name == tagName(Tag(t)))
            settings.brief.tags.push_back(Tag(t));
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
  std::cout << "site " << (selected ? "selected" : "rejected") << " " << error << "\n";
  json report;
  report["world"] = {{"seed", settings.brief.seed}, {"extentMetres", settings.brief.extentMetres}, {"cellSpacingMetres", settings.cellSpacingMetres}, {"reliefMetres", settings.brief.reliefMetres}, {"leafCells", world.leafCells}, {"landCells", world.landCells}};
  report["siteSelected"] = selected;
  report["siteError"] = error;
  const double extent = settings.brief.extentMetres;
  // Overview: every leaf by biome, rivers, all candidates, the chosen domain.
  {
    const double size = 1400;
    Svg svg(size, size, size / extent);
    for (const auto &n : world.graph.nodes())
    {
      if (n.refined())
        continue;
      Poly p;
      for (auto q : n.area)
        p.push_back({q.x, 0, q.y});
      svg.polygon(p, biomeColour(biomeOf(n)), "#ffffff", .3);
    }
    for (const auto &s : world.rivers.segments)
      svg.polyline({{s.from.x, 0, s.from.y}, {s.to.x, 0, s.to.y}}, "#3d7fd0", .6 + std::sqrt(std::max(0., s.dischargeCumecs)) * .15);
    for (const auto &c : site.candidates)
      svg.circle(c.centre, c.accepted ? 3.5 : 2.5, c.accepted ? "#2ecc40" : "#e0342a", "#222", .4, .85);
    if (selected)
    {
      svg.polygon({site.origin, {site.origin.x + site.bounds.maxX, 0, site.origin.z}, {site.origin.x + site.bounds.maxX, 0, site.origin.z + site.bounds.maxZ}, {site.origin.x, 0, site.origin.z + site.bounds.maxZ}}, "none", "#ff9900", 2.5);
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
        fill = "#7fb2e5";
      else if (c.reasons & (Steep | Cliff | Brink))
        fill = "#c49a6c";
      else if (c.reasons & Rough)
        fill = "#b8a48a";
      else if (c.reasons & PoorSoil)
        fill = "#e6dca0";
      else if (c.reasons & Saturated)
        fill = "#9fc7b8";
      else if (c.reasons & Flood)
        fill = "#a9c4e8";
      else if (c.reasons & Inaccessible)
        fill = "#d5d5d5";
      else
        continue;
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
      for (const auto &line : contourLines(heights, w.columns(), w.rows(), 0, 0, h, level))
        svg.polyline(line, "#6b5a3a", std::fmod(level, interval * 5) == 0 ? 1 : .5, "stroke-opacity=\"0.6\"");
    for (const auto &r : site.regions)
    {
      std::string fill = r.cover == RuralLandCover::Forest ? "#4f7a3a" : r.cover == RuralLandCover::Rock ? "#8a8a8a" : r.cover == RuralLandCover::Water ? "#5f9ad6" : "none";
      svg.polygon(r.boundary, fill, "#8c7a4a", .8, .5);
    }
    for (const auto &p : territory.rural.patches)
      svg.polygon(p.boundary, "#e9d58a", "#9a7d1c", 1.2, .55);
    for (const auto &f : territory.fields)
      for (const auto &field : f.fields)
        svg.polygon(field.boundary, "#d9b85c", "#7a5f10", .6, .85);
    for (const auto &c : territory.surfaces.contours)
      if (c.role == SurfaceRole::Road)
        svg.polygon(c.loop, "#b28b5a", "#6a4f2a", .3, .95);
    for (const auto &p : territory.rural.patches)
      svg.polyline(p.access, "#7a5f10", 1.2, "stroke-dasharray=\"4 3\"");
    for (const auto &hold : territory.holdings)
    {
      svg.polygon(hold.yard, "#a0703c", "#5a3a1a", .8, .9);
      for (const auto &b : hold.buildings)
        svg.polygon(b.footprint, "#4a2e12", "#000", .4);
    }
    for (const auto &cell : site.brief.input.cells)
      svg.polygon(cell.boundary, cell.buildable ? "none" : "#c0c0c0", "#444", .7, .5);
    for (const auto &d : territory.regions.districts)
    {
      for (const auto &piece : d.pieces)
        svg.polygon(piece, useColour(d.use), "#222", 1, .75);
      svg.polyline(d.access, "#111", 1.5, "stroke-dasharray=\"6 4\"");
      if (!d.pieces.empty())
        svg.text(centroid(d.pieces.front()), districtUseName(d.use), 11, "#000");
    }
    for (const auto &g : site.brief.input.gateCells)
      for (const auto &cell : site.brief.input.cells)
        if (cell.id == g)
          svg.circle(centroid(cell.boundary), 6, "#ffffff", "#000", 1.5);
    for (const auto &src : site.brief.input.ruralSources)
      svg.circle(src.exit, 6, "#ffcc00", "#000", 1.5);
    svg.circle(site.localCentre, 8, "#e0342a", "#000", 1.5);
    svg.text({20, 0, site.bounds.maxZ - 30}, "site " + std::to_string(site.cell) + " (" + site.regionName + ")  contour " + std::to_string(int(interval)) + " m  population " + std::to_string(site.demand.population), 16, "#000", "start");
    std::ofstream(output / "domain.svg") << svg.finish();
  }
  // JSON ledger.
  report["site"] = {{"id", site.id}, {"cell", site.cell}, {"region", site.regionName}, {"centre", poly({site.centre})}, {"origin", poly({site.origin})}, {"size", {site.bounds.maxX, site.bounds.maxZ}}, {"attempts", site.attempts}, {"population", site.demand.population}, {"households", site.households.size()}, {"planningArea", site.planningArea}, {"hinterlandArea", site.hinterlandArea}, {"openArea", site.openArea}};
  for (const auto &c : site.candidates)
    report["candidates"].push_back({{"cell", c.cell}, {"centre", poly({c.centre})}, {"score", c.score}, {"gradient", c.gradient}, {"hinterlandGradient", c.hinterlandGradient}, {"waterDistance", c.waterDistance}, {"fertility", c.fertility}, {"biome", biomeName(Biome(c.biome))}, {"accepted", c.accepted}, {"rejection", c.rejection}});
  report["omissions"] = site.rejections;
  for (const auto &cell : site.brief.input.cells)
    report["planningCells"].push_back({{"id", cell.id}, {"boundary", poly(cell.boundary)}, {"slope", cell.slope}, {"floodRisk", cell.floodRisk}, {"fertility", cell.fertility}, {"defense", cell.defense}, {"buildable", cell.buildable}, {"navigableShore", cell.navigableShore}});
  for (const auto &r : site.regions)
    report["regions"].push_back({{"id", r.id}, {"boundary", poly(r.boundary)}, {"cover", int(r.cover)}});
  for (const auto &src : site.brief.input.ruralSources)
    report["ruralSources"].push_back({{"id", src.id}, {"exit", poly({src.exit})}, {"areaBudget", src.areaBudget}});
  report["gateCells"] = site.brief.input.gateCells;
  for (const auto &d : site.brief.input.districts)
    report["districtRequests"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"minimumArea", d.minimumArea}, {"targetArea", d.targetArea}});
  report["suitability"] = {{"columns", w.columns()}, {"rows", w.rows()}, {"spacing", h}, {"fingerprint", w.fingerprint()}};
  {
    std::string reasons;
    for (const auto &c : w.cells())
      reasons += c.reasons ? '1' : '0';
    report["suitability"]["eligible"] = reasons;
  }
  if (composed)
  {
    report["territory"] = {{"urbanArea", territory.regions.assignedArea}, {"cropArea", territory.cropArea}, {"farmyardArea", territory.farmyardArea}, {"patches", territory.rural.patches.size()}, {"holdings", territory.holdings.size()}, {"contours", territory.surfaces.contours.size()}, {"eligibleArea", territory.rural.eligibleArea}, {"patchArea", territory.rural.patchArea}, {"unallocatedArea", territory.rural.unallocatedArea}};
    for (const auto &d : territory.regions.districts)
    {
      json pieces = json::array();
      for (auto q : d.pieces)
        pieces.push_back(poly(q));
      report["districts"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"cells", d.cells}, {"pieces", pieces}, {"access", poly(d.access)}, {"area", d.area}, {"shortfall", d.unmetTargetArea}});
    }
    for (const auto &o : territory.regions.omissions)
      report["districtOmissions"].push_back({{"request", o.request}, {"reason", o.reason}});
    for (size_t i = 0; i < territory.rural.patches.size(); ++i)
    {
      const auto &p = territory.rural.patches[i];
      json fields = json::array();
      for (const auto &f : territory.fields[i].fields)
        fields.push_back({{"boundary", poly(f.boundary)}, {"area", f.area}});
      report["patches"].push_back({{"id", p.id}, {"source", p.source}, {"originalRegion", p.originalRegion}, {"boundary", poly(p.boundary)}, {"access", poly(p.access)}, {"area", p.area}, {"cropArea", territory.fields[i].cropArea}, {"fields", fields}});
    }
    for (const auto &hold : territory.holdings)
    {
      json buildings = json::array();
      for (const auto &b : hold.buildings)
        buildings.push_back({{"id", b.id}, {"use", int(b.use)}, {"footprint", poly(b.footprint)}, {"height", b.height}});
      report["holdings"].push_back({{"patch", hold.patch}, {"yard", poly(hold.yard)}, {"buildings", buildings}});
    }
    report["ruralRejections"] = territory.rural.rejections;
    report["budgets"] = json::array();
    for (const auto &b : territory.rural.budgets)
      report["budgets"].push_back({{"source", b.source}, {"requested", b.requested}, {"allocated", b.allocated}, {"shortfall", b.shortfall}});
  }
  std::ofstream(output / "settlement.json") << report.dump(2);
  // HTML.
  {
    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>World settlement W0 preview</title><style>body{font-family:Helvetica,Arial,sans-serif;margin:20px;color:#222}table{border-collapse:collapse;margin:8px 0}td,th{border:1px solid #ccc;padding:3px 8px;font-size:13px;text-align:left}img{max-width:100%;border:1px solid #999}.legend span{display:inline-block;width:14px;height:14px;margin-right:4px;vertical-align:middle;border:1px solid #666}</style></head><body>";
    html << "<h1>Actual-world settlement, W0 input adapter</h1><p>Seed " << settings.brief.seed << ", " << extent / 1000 << " km world, " << settings.cellSpacingMetres << " m leaf cells, relief " << settings.brief.reliefMetres << " m. Site cell " << site.cell << " in " << site.regionName << ", population brief " << site.demand.population << ".</p>";
    html << "<h2>World overview</h2><p>Leaf cells by biome, rivers by discharge. Green dots: accepted site candidates; red: rejected with a recorded reason. Orange box: the settlement domain (the urban cell plus its neighbouring map cells).</p><img src=\"overview.svg\">";
    html << "<h2>Settlement domain (local frame, " << site.bounds.maxX << " x " << site.bounds.maxZ << " m)</h2><p class=\"legend\"><span style=\"background:#7fb2e5\"></span>explicit water <span style=\"background:#c49a6c\"></span>steep <span style=\"background:#b8a48a\"></span>rough <span style=\"background:#e6dca0\"></span>poor soil <span style=\"background:#9fc7b8\"></span>saturated <span style=\"background:#d5d5d5\"></span>unreachable <span style=\"background:#4f7a3a\"></span>forest region <span style=\"background:#8a8a8a\"></span>rock region <span style=\"background:#e9d58a\"></span>allocated farmland <span style=\"background:#d9b85c\"></span>crop field <span style=\"background:#b28b5a\"></span>lane <span style=\"background:#a0703c\"></span>farmyard <span style=\"background:#e07b5f\"></span>residential <span style=\"background:#d9a441\"></span>craftsmen <span style=\"background:#c56f9a\"></span>worker housing <span style=\"background:#8e6bd6\"></span>temple <span style=\"background:#4a8fd6\"></span>civic. White circles: gate cells; yellow: road exits; red: site centre; dashed black: planning access; dashed brown: farm access; thin brown: elevation contours.</p><img src=\"domain.svg\">";
    html << "<h2>Ledger</h2><table><tr><th>Metric</th><th>Value</th></tr>";
    html << "<tr><td>Candidates evaluated</td><td>" << site.candidates.size() << "</td></tr><tr><td>Planning cells</td><td>" << site.brief.input.cells.size() << "</td></tr><tr><td>Hinterland regions</td><td>" << site.regions.size() << "</td></tr><tr><td>Road exits / gates</td><td>" << site.brief.input.ruralSources.size() << " / " << site.brief.input.gateCells.size() << "</td></tr>";
    if (composed)
      html << "<tr><td>Districts allocated</td><td>" << territory.regions.districts.size() << " (" << territory.regions.assignedArea << " m&sup2;)</td></tr><tr><td>Farm patches / fields / crop area</td><td>" << territory.rural.patches.size() << " / " << [&] { size_t n = 0; for (auto &f : territory.fields) n += f.fields.size(); return n; }() << " / " << territory.cropArea / 1e4 << " ha</td></tr><tr><td>Farmsteads</td><td>" << territory.holdings.size() << "</td></tr><tr><td>Rural rejections</td><td>" << territory.rural.rejections.size() << "</td></tr>";
    else
      html << "<tr><td>Territory</td><td>rejected: " << error << "</td></tr>";
    html << "</table><h3>Rejected site candidates (first 12)</h3><table><tr><th>Cell</th><th>Score</th><th>Reason</th></tr>";
    size_t shown = 0;
    for (const auto &c : site.candidates)
      if (!c.accepted && shown++ < 12)
        html << "<tr><td>" << c.cell << "</td><td>" << c.score << "</td><td>" << c.rejection << "</td></tr>";
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
        html << "<tr><td>" << k << "</td><td>" << v << "</td></tr>";
      html << "</table>";
    }
    html << "</body></html>";
    std::ofstream(output / "index.html") << html.str();
  }
  std::cout << "wrote " << output << "\n";
  return composed ? 0 : 3;
}
