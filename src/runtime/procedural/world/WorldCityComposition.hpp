#pragma once
#include "WorldCityCompletion.hpp"
#include "WorldCityStructures.hpp"
#include "hierarchy/CellSkeleton.hpp"

namespace procedural::world::rewrite {
enum class CityNeighbourhoodPalette
{
  WarmTimber,
  SlateStone,
  GreenMarket
};
enum class CityFunctionalProp
{
  Bench,
  MarketStall,
  Signboard
};

struct CityCompositionBlock
{
  uint64_t block = 0;
  CityNeighbourhoodPalette palette = CityNeighbourhoodPalette::WarmTimber;
  std::vector<Position> boundary;
  std::vector<std::vector<Position>> publicSpaces;
};

struct CityLandmarkRequest
{
  uint64_t building = 0;
  std::vector<Position> cameras;
};

struct CityPropRequest
{
  uint64_t id = 0, building = 0;
  CityFunctionalProp kind = CityFunctionalProp::Bench;
  std::vector<Position> candidates;
};

struct CityCompositionSettings
{
  double minimumBuildingGap = .2;
  double minimumCoverage = .35, maximumCoverage = .85, minimumPublicSpace = .1;
  double maximumFrontageDeviation = 1.2, minimumLandmarkProminence = 1., minimumVisibleFraction = .6;
  double minimumDoorWidth = .85, minimumDoorHeight = 1.9, minimumWindowWidth = .45, minimumWindowHeight = .8;
  size_t maximumPropAttempts = 256;
};

struct CityBuildingStyle
{
  uint64_t building = 0, block = 0;
  uint32_t roof = 2, wall = 4;
  bool landmark = false;
};

struct CityPropPart
{
  CityStructureBox box;
  uint32_t material = 3;
};

struct CityPlannedProp
{
  uint64_t id = 0, building = 0;
  CityFunctionalProp kind = CityFunctionalProp::Bench;
  CityStructureBox bounds, interaction;
  std::vector<CityPropPart> parts;
};

struct CityCompositionMetric
{
  uint64_t block = 0;
  double coverage = 0, publicSpace = 0, frontageDeviation = 0;
};

struct CityLandmarkView
{
  uint64_t building = 0;
  Position camera;
  double visibleFraction = 0;
};

struct CityCompositionPlan
{
  uint64_t revision = 0;
  std::shared_ptr<const CityPlotContractPlan> plots;
  std::shared_ptr<const CityStructurePlan> structures;
  std::vector<CityBuildingStyle> buildings;
  std::vector<CityPlannedProp> props;
  std::vector<CityCompositionMetric> metrics;
  std::vector<CityLandmarkView> views;
  size_t propAttempts = 0;
};

namespace city_composition_detail {
using Box = CityStructureBox;

inline bool finite(Position p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

inline Box bounds(const CityPlotContract &p)
{
  Box b{INFINITY, p.floor, INFINITY, -INFINITY, p.floor, -INFINITY};
  auto point = [&](Position v) {
    b.x0 = std::min(b.x0, v.x);
    b.x1 = std::max(b.x1, v.x);
    b.z0 = std::min(b.z0, v.z);
    b.z1 = std::max(b.z1, v.z);
  };
  for (const auto &s : p.dwelling.storeys)
  {
    for (auto v : s.outer)
    {
      point(v);
    }
    b.y1 = std::max(b.y1, p.floor + s.top);
  }
  for (const auto &r : p.dwelling.roofs)
  {
    for (auto v : r.polygon)
    {
      point(v);
      b.y1 = std::max(b.y1, p.floor + v.y);
    }
  }
  auto skeleton = hierarchy::buildSkeleton(p.dwelling);
  hierarchy::Solid mesh;
  hierarchy::meshSkeleton(skeleton, mesh);
  const auto group = [&](const auto &faces) {
    for (const auto &face : faces)
    {
      for (auto v : face)
      {
        point({v.x, v.y, v.z});
        b.y1 = std::max(b.y1, p.floor + v.y);
      }
    }
  };
  group(mesh.walls);
  group(mesh.gables);
  group(mesh.slopes);
  group(mesh.joinery);
  group(mesh.timbers);
  group(mesh.dormerWalls);
  group(mesh.dormerWindows);
  group(mesh.dormerRoofs);
  group(mesh.pillars);
  group(mesh.soffits);
  return b;
}

inline bool ray(Box b, Position a, Position end)
{
  double lo = 0, hi = 1;
  double av[] = {a.x, a.y, a.z}, dv[] = {end.x - a.x, end.y - a.y, end.z - a.z}, mn[] = {b.x0, b.y0, b.z0}, mx[] = {b.x1, b.y1, b.z1};
  for (unsigned k = 0; k < 3; ++k)
  {
    if (std::abs(dv[k]) < 1e-9)
    {
      if (av[k] < mn[k] || av[k] > mx[k])
      {
        return false;
      }
    }
    else
    {
      double u = (mn[k] - av[k]) / dv[k], v = (mx[k] - av[k]) / dv[k];
      if (u > v)
      {
        std::swap(u, v);
      }
      lo = std::max(lo, u);
      hi = std::min(hi, v);
      if (hi < lo)
      {
        return false;
      }
    }
  }
  return hi > 1e-5 && lo < 1 - 1e-5;
}

inline bool inside(const std::vector<Position> &outer, const std::vector<Position> &inner)
{
  for (auto p : inner)
  {
    if (!city_terrain_detail::inside(outer, p))
    {
      return false;
    }
  }
  return true;
}
} // namespace city_composition_detail

// Composition accepts or rejects the complete inherited layout. It cannot move
// a floor, shrink a module or silently delete an unmet historical programme.
inline bool planCityComposition(
    std::shared_ptr<const CityPlotContractPlan> plots,
    std::shared_ptr<const CityStructurePlan> structures,
    std::vector<CityCompositionBlock> blocks,
    std::vector<CityLandmarkRequest> landmarks,
    std::vector<CityPropRequest> props,
    const CityCompositionSettings &settings,
    CityCompositionPlan &result,
    std::string *error = nullptr)
{
  using namespace city_composition_detail;
  using namespace city_structure_detail;
  auto fail = [&](const std::string &s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  if (!plots || !plots->revision || !plots->terrain || !plots->terrain->ground || plots->plots.empty() || (structures && (structures->plots != plots || structures->revision != plots->revision)))
  {
    return fail("composition requires accepted plot/structure snapshots");
  }
  if (!std::isfinite(
          settings.minimumBuildingGap + settings.minimumCoverage + settings.maximumCoverage + settings.minimumPublicSpace + settings.maximumFrontageDeviation + settings.minimumLandmarkProminence +
          settings.minimumVisibleFraction + settings.minimumDoorWidth + settings.minimumDoorHeight + settings.minimumWindowWidth + settings.minimumWindowHeight) ||
      settings.minimumBuildingGap < .2 || settings.minimumCoverage <= 0 || settings.maximumCoverage >= 1 || settings.minimumCoverage > settings.maximumCoverage || settings.minimumPublicSpace <= 0 ||
      settings.minimumPublicSpace + settings.minimumCoverage > 1 || settings.maximumFrontageDeviation < 0 || settings.minimumLandmarkProminence < 0 || settings.minimumVisibleFraction <= 0 ||
      settings.minimumVisibleFraction > 1 || settings.minimumDoorWidth < .85 || settings.minimumDoorHeight < 1.9 || settings.minimumWindowWidth < .45 || settings.minimumWindowHeight < .8 || !settings.maximumPropAttempts)
  {
    return fail("invalid composition policy");
  }
  if (blocks.size() > 1024 || landmarks.size() > 256 || props.size() > 4096)
  {
    return fail("composition programme budget exceeded");
  }
  CityCompositionPlan p;
  p.revision = plots->revision;
  p.plots = plots;
  p.structures = structures;
  std::sort(blocks.begin(), blocks.end(), [](auto &a, auto &b) {
    return a.block < b.block;
  });
  std::sort(props.begin(), props.end(), [](auto &a, auto &b) {
    return a.id < b.id;
  });
  std::sort(landmarks.begin(), landmarks.end(), [](auto &a, auto &b) {
    return a.building < b.building;
  });
  std::map<uint64_t, const CityPlotContract *> lots;
  std::map<uint64_t, Box> buildings;
  std::set<uint64_t> ids, assigned;
  for (const auto &lot : plots->plots)
  {
    if (!lots.emplace(lot.building, &lot).second)
    {
      return fail("duplicate composed building");
    }
    buildings[lot.building] = bounds(lot);
    if (!valid(buildings[lot.building]))
    {
      return fail("invalid dwelling envelope");
    }
    if (std::abs(lot.house.frame.cellMetres - lot.module) > 1e-7)
    {
      return fail("dwelling module was squeezed");
    }
    for (const auto &o : lot.dwelling.openings)
    {
      if (o.exterior && (o.kind == OpeningKind::Window || o.kind == OpeningKind::Door))
      {
        double w = std::hypot(o.b.x - o.a.x, o.b.z - o.a.z), h = std::min(o.top, o.head) - std::max(o.base, o.sill);
        bool door = o.kind == OpeningKind::Door;
        if (!std::isfinite(w + h) || w + 1e-6 < (door ? settings.minimumDoorWidth : settings.minimumWindowWidth) || h + 1e-6 < (door ? settings.minimumDoorHeight : settings.minimumWindowHeight))
        {
          return fail("undersized opening in building " + std::to_string(lot.building));
        }
      }
    }
  }
  // Conservative complete roof/storey bounds forbid even decorative overhangs
  // colliding across adjacent plots. Touching party boundaries are permitted.
  for (auto a = buildings.begin(); a != buildings.end(); ++a)
  {
    for (auto b = std::next(a); b != buildings.end(); ++b)
    {
      if ([&] {
            auto expanded = a->second;
            expanded.x0 -= settings.minimumBuildingGap;
            expanded.x1 += settings.minimumBuildingGap;
            expanded.z0 -= settings.minimumBuildingGap;
            expanded.z1 += settings.minimumBuildingGap;
            return overlap(expanded, b->second);
          }())
      {
        return fail("building envelopes collide; replan frontage");
      }
    }
  }
  if (structures)
  {
    for (const auto &[id, b] : buildings)
    {
      for (const auto &solid : structures->solids)
      {
        if (overlap(b, solid.box))
        {
          return fail("building envelope collides with structural assembly");
        }
      }
    }
  }
  for (const auto &block : blocks)
  {
    if (!block.block || !ids.insert(block.block).second || !city_terrain_detail::convex(block.boundary) || unsigned(block.palette) > 2)
    {
      return fail("invalid neighbourhood block");
    }
    double area = city_plot_detail::area(block.boundary), occupied = 0, space = 0, near = INFINITY, far = 0;
    size_t count = 0;
    for (const auto &lot : plots->plots)
    {
      if (lot.block == block.block)
      {
        if (!inside(block.boundary, lot.lot))
        {
          return fail("plot outside composition block");
        }
        if (!inside(block.boundary, footprint(buildings.at(lot.building))))
        {
          return fail("native building projection exceeds neighbourhood reservation");
        }
        assigned.insert(lot.building);
        ++count;
        occupied += city_plot_detail::area(lot.envelope);
        double setback = std::hypot(lot.door.x - lot.street.x, lot.door.z - lot.street.z);
        near = std::min(near, setback);
        far = std::max(far, setback);
        CityBuildingStyle style;
        style.building = lot.building;
        style.block = block.block;
        if (block.palette == CityNeighbourhoodPalette::WarmTimber)
        {
          style.roof = 2;
          style.wall = 16;
        }
        if (block.palette == CityNeighbourhoodPalette::SlateStone)
        {
          style.roof = 20;
          style.wall = 1;
        }
        if (block.palette == CityNeighbourhoodPalette::GreenMarket)
        {
          style.roof = 25;
          style.wall = 4;
        }
        p.buildings.push_back(style);
      }
    }
    if (!count)
    {
      return fail("empty composition block");
    }
    for (size_t i = 0; i < block.publicSpaces.size(); ++i)
    {
      auto &s = block.publicSpaces[i];
      if (!city_terrain_detail::convex(s) || !inside(block.boundary, s))
      {
        return fail("invalid public space");
      }
      for (size_t j = 0; j < i; ++j)
      {
        if (city_plot_detail::overlap(s, block.publicSpaces[j]))
        {
          return fail("public spaces double counted");
        }
      }
      bool access = false;
      for (const auto &lot : plots->plots)
      {
        if (city_plot_detail::overlap(s, lot.envelope))
        {
          return fail("public space invades dwelling");
        }
        if (lot.block == block.block)
        {
          access |= city_plot_detail::overlap(s, lot.approach);
        }
      }
      if (!access)
      {
        return fail("public space lacks a reserved doorway connection");
      }
      space += city_plot_detail::area(s);
    }
    CityCompositionMetric m{block.block, occupied / area, space / area, far - near};
    if (m.coverage + 1e-6 < settings.minimumCoverage || m.coverage > settings.maximumCoverage + 1e-6)
    {
      return fail("neighbourhood density target requires plot replanning");
    }
    if (m.publicSpace + 1e-6 < settings.minimumPublicSpace)
    {
      return fail("insufficient reserved public space");
    }
    if (m.frontageDeviation > settings.maximumFrontageDeviation + 1e-6)
    {
      return fail("fronts exceed agreed alignment band");
    }
    p.metrics.push_back(m);
  }
  if (assigned.size() != plots->plots.size())
  {
    return fail("composition omitted accepted buildings");
  }
  ids.clear();
  for (const auto &request : props)
  {
    if (!request.id || !ids.insert(request.id).second || !lots.count(request.building) || request.candidates.empty() || request.candidates.size() > 128 || unsigned(request.kind) > 2)
    {
      return fail("invalid functional prop request");
    }
    bool accepted = false;
    auto owner = lots.at(request.building);
    auto block = std::find_if(blocks.begin(), blocks.end(), [&](auto &b) {
      return b.block == owner->block;
    });
    for (auto pos : request.candidates)
    {
      if (++p.propAttempts > settings.maximumPropAttempts)
      {
        return fail("functional prop search budget exhausted");
      }
      if (!finite(pos))
      {
        return fail("nonfinite prop candidate");
      }
      double width = request.kind == CityFunctionalProp::MarketStall ? 2.4
                     : request.kind == CityFunctionalProp::Bench     ? 1.6
                                                                     : .6,
             depth = request.kind == CityFunctionalProp::MarketStall ? 1.2 : .5,
             height = request.kind == CityFunctionalProp::MarketStall ? 2.4
                      : request.kind == CityFunctionalProp::Bench     ? .9
                                                                      : 1.3;
      double low = INFINITY, high = -INFINITY;
      for (double x : {-.5, 0., .5})
      {
        for (double z : {-.5, 0., .5})
        {
          double y = plots->terrain->ground(pos.x + x * width, pos.z + z * depth);
          if (!std::isfinite(y))
          {
            return fail("nonfinite prop ground");
          }
          low = std::min(low, y);
          high = std::max(high, y);
        }
      }
      if (high - low > .04)
      {
        continue;
      }
      CityPlannedProp prop;
      prop.id = request.id;
      prop.building = request.building;
      prop.kind = request.kind;
      prop.bounds = {pos.x - width * .5, high, pos.z - depth * .5, pos.x + width * .5, high + height, pos.z + depth * .5};
      prop.interaction = {prop.bounds.x0, high, prop.bounds.z1, prop.bounds.x1, high + 2.1, prop.bounds.z1 + .9};
      bool clear = false;
      for (auto &s : block->publicSpaces)
      {
        clear |= inside(s, footprint(prop.bounds)) && inside(s, footprint(prop.interaction));
      }
      if (!clear)
      {
        continue;
      }
      for (auto &[id, b] : buildings)
      {
        clear &= !overlap(b, prop.bounds) && !overlap(b, prop.interaction);
      }
      for (auto &lot : plots->plots)
      {
        clear &= !city_plot_detail::overlap(footprint(prop.bounds), lot.approach);
      }
      for (auto &old : p.props)
      {
        clear &= !overlap(old.bounds, prop.bounds) && !overlap(old.bounds, prop.interaction) && !overlap(old.interaction, prop.bounds);
      }
      if (structures)
      {
        for (auto &s : structures->solids)
        {
          clear &= !overlap(s.box, prop.bounds) && !overlap(s.box, prop.interaction);
        }
        for (auto b : structures->clearance)
        {
          clear &= !overlap(b, prop.bounds);
        }
      }
      for (double u : {.1, .5, .9})
      {
        for (double v : {.1, .5, .9})
        {
          double y = plots->terrain->ground(prop.interaction.x0 + (prop.interaction.x1 - prop.interaction.x0) * u, prop.interaction.z0 + (prop.interaction.z1 - prop.interaction.z0) * v);
          clear &= std::isfinite(y) && std::abs(y - high) <= .04;
        }
      }
      if (!clear)
      {
        continue;
      }
      auto b = prop.bounds;
      auto part = [&](Box q, uint32_t material = 3) {
        prop.parts.push_back({q, material});
      };
      if (request.kind == CityFunctionalProp::Signboard)
      {
        part({pos.x - .06, high, pos.z - .06, pos.x + .06, high + height, pos.z + .06});
        part({b.x0, high + .6, pos.z - .08, b.x1, b.y1, pos.z + .08});
      }
      else
      {
        double top = request.kind == CityFunctionalProp::Bench ? .45 : .9;
        part({b.x0, high + top - .1, b.z0, b.x1, high + top, b.z1});
        for (double x : {b.x0 + .1, b.x1 - .2})
        {
          for (double z : {b.z0 + .05, b.z1 - .15})
          {
            part({x, high, z, x + .1, high + top, z + .1});
          }
        }
        if (request.kind == CityFunctionalProp::Bench)
        {
          part({b.x0, high + .45, b.z0, b.x1, b.y1, b.z0 + .1});
        }
        else
        {
          for (double x : {b.x0, b.x1 - .08})
          {
            part({x, high, b.z0, x + .08, b.y1, b.z0 + .08});
          }
          part({b.x0, b.y1 - .08, b.z0, b.x1, b.y1, b.z1}, 4);
        }
      }
      p.props.push_back(std::move(prop));
      accepted = true;
      break;
    }
    if (!accepted)
    {
      return fail("no safe functional prop placement for " + std::to_string(request.id));
    }
  }
  ids.clear();
  for (const auto &request : landmarks)
  {
    if (!ids.insert(request.building).second || !lots.count(request.building) || request.cameras.empty() || request.cameras.size() > 16)
    {
      return fail("invalid landmark view programme");
    }
    const auto target = buildings.at(request.building);
    auto lot = lots.at(request.building);
    for (auto &[id, b] : buildings)
    {
      if (id != request.building && lots.at(id)->block == lot->block && target.y1 < b.y1 + settings.minimumLandmarkProminence)
      {
        return fail("landmark lacks skyline prominence");
      }
    }
    for (auto camera : request.cameras)
    {
      if (!finite(camera) || camera.y <= plots->terrain->ground(camera.x, camera.z) + .1)
      {
        return fail("invalid approach camera");
      }
      unsigned visible = 0;
      std::vector<Position> samplesOnRoof;
      for (const auto &roof : lot->dwelling.roofs)
      {
        for (auto v : roof.polygon)
        {
          samplesOnRoof.push_back({v.x, v.y + lot->floor, v.z});
        }
      }
      std::sort(samplesOnRoof.begin(), samplesOnRoof.end(), [](auto a, auto b) {
        return a.y != b.y ? a.y > b.y : a.x != b.x ? a.x < b.x : a.z < b.z;
      });
      samplesOnRoof.erase(
          std::unique(
              samplesOnRoof.begin(),
              samplesOnRoof.end(),
              [](auto a, auto b) {
                return std::hypot(a.x - b.x, a.z - b.z) < 1e-6 && std::abs(a.y - b.y) < 1e-6;
              }),
          samplesOnRoof.end());
      if (samplesOnRoof.empty())
      {
        return fail("landmark has no real roof silhouette");
      }
      samplesOnRoof.resize(std::min(size_t(3), samplesOnRoof.size()));
      for (Position aim : samplesOnRoof)
      {
        bool seen = true;
        for (auto &[id, b] : buildings)
        {
          if (id != request.building)
          {
            seen &= !ray(b, camera, aim);
          }
        }
        for (auto &prop : p.props)
        {
          seen &= !ray(prop.bounds, camera, aim);
        }
        if (structures)
        {
          for (auto &solid : structures->solids)
          {
            seen &= !ray(solid.box, camera, aim);
          }
        }
        double distance = std::hypot(camera.x - aim.x, camera.z - aim.z);
        if (distance > 2000)
        {
          return fail("landmark visibility budget exceeded");
        }
        unsigned samples = std::max(2u, unsigned(std::ceil(distance / .5)));
        for (unsigned i = 1; i < samples && seen; ++i)
        {
          auto point = city_terrain_detail::lerp(camera, aim, double(i) / samples);
          double ground = plots->terrain->ground(point.x, point.z);
          seen = std::isfinite(ground) && point.y > ground + .05;
        }
        visible += seen;
      }
      double fraction = double(visible) / samplesOnRoof.size();
      if (fraction + 1e-6 < settings.minimumVisibleFraction)
      {
        return fail("landmark obscured from required approach");
      }
      p.views.push_back({request.building, camera, fraction});
    }
    for (auto &style : p.buildings)
    {
      if (style.building == request.building)
      {
        style.landmark = true;
      }
    }
  }
  std::sort(p.buildings.begin(), p.buildings.end(), [](auto &a, auto &b) {
    return a.building < b.building;
  });
  result = std::move(p);
  if (error)
  {
    error->clear();
  }
  return true;
}

inline bool applyCityComposition(const CityCompositionPlan &composition, CityCompletionPlan &city, std::string *error = nullptr)
{
  if (!composition.plots || composition.buildings.empty() || composition.buildings.size() != composition.plots->plots.size() || !composition.revision || composition.revision != city.revision ||
      composition.plots != city.plotContracts || composition.structures != city.structureContracts)
  {
    if (error)
    {
      *error = "composition requires exact plot and structure snapshots";
    }
    return false;
  }
  city.composition = std::make_shared<const CityCompositionPlan>(composition);
  if (error)
  {
    error->clear();
  }
  return true;
}
} // namespace procedural::world::rewrite
